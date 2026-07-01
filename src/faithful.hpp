// ztpp — FAITHFUL рендер: воспроизводит проекцию/растеризацию ZT (render3d.asm).
//
// По дизасму (ZT_complete.asm ca04/c9a8/ca4c/ccb6):
//   forward = dx*cos + dy*sin ; lateral = dy*cos - dx*sin   (dx=X-camX, dy=Y-camY)
//   screenX(0..127) = 0x40 + 0x40 * lateral/forward         (центр 64, FOV ~90°)
//   scale = 0x10000/(forward>>6) = 64/forward = ЦЕЛОЕ D0 (высота колонны)
//   scale (высота) интерполируется линейно по экрану (= перспективно-корректно для глубины);
//   texU — перспективно-корректный (u/z, 1/z, деление): иначе аффинный texU «плывёт» на близких
//   наклонных стенах при вращении (ZT технически аффинный, но это заметнее — выбрали гладкость).
// СКЕЙЛЕР БИТ-В-БИТ как компилированные рутины ZT (0x4766..0x700c): колонна = ровно D0 пикс,
// тексель строки i = floor((i+0.5)*64/D0) (центр-сэмплинг 64-стр. источника = 2 тайла). Доказано
// сравнением с офсетами рутин (H=8/16/24/32/36 совпали). Банд по D0 (bandForHeight) — точная таблица.
// Ячейка-проектор: проецируются РЁБРА клеток-стен. Нативное 128px (чанки ZT), апскейл в framebuffer.
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"
#include "cells.hpp"
#include "raycaster.hpp"   // Camera, MetaCache, shade, rcMaxVis, cellIsDoor, DOOR_METATEX
#include <cmath>
#include <cstdint>
#include <vector>

// Параметры проекции ZT (из ccb6 / sin-cos @0x8124, M=256).
// Высота колонки = scale = 0x10000/(forward>>6); по геометрии стена 1×1 клетка рендерится
// КВАДРАТНО (height==width). Прямоугольный вид в игре = вертикальная растяжка дисплея MD.
static const int    FA_NW = 128;       // ширина нат-буфера по умолчанию (фуллскрин)
static const double FA_FOCAL = 64.0;   // дефолтный фокус
static const double FA_NEAR = 0.02;

// Раздельные ФОКУСЫ (нативный рендер ZT = 256×80, НЕквадратный пиксель 2:1): горизонтальный фокус
// = natW/2 (FOV 90° по ширине), вертикальный = 64 (высота колонны D0=64/forward, макс 80 = вся высота
// вида). Для фуллскрина natW=128→focalH=64; для РЕФЕРЕНСА natW=256→focalH=128, natH=80. Ставятся в
// renderFaithful, читаются в faDrawSeg (как faHStretch).
inline double& faFocalH() { static double v = 64.0; return v; }   // гор. фокус (screenX)
inline double& faFocalV() { static double v = 64.0; return v; }   // верт. фокус (высота D0)
// faDrawDist() — в raycaster.hpp (нужен и renderFPS, который объявлен раньше).

// faHStretch(), ShadeRamps, bandForHeight — общие, объявлены в raycaster.hpp.
// Освещение faithful = ТОЧНАЯ таблица ZT height→band (bandForHeight) + рампы (ShadeRamps) с
// дизером hi/lo. Bright(env0) — без рампы. Никаких аппроксимаций/Bayer.

// Проекция мировой точки (в клетках) -> forward (вдоль взгляда), lateral (поперёк).
inline void faProject(const Camera& c, double wx, double wy, double& fwd, double& lat) {
    double rx = wx - c.px, ry = wy - c.py;
    fwd = rx * c.dirX + ry * c.dirY;
    lat = ry * c.dirX - rx * c.dirY;   // dy*cos - dx*sin
}

// Растеризация одного ребра-грани в нативный буфер nat (NW x NH) с z-тестом по столбцам.
// zc[x] хранит лучший (ближайший) 1/forward. faceMeta — индекс метатекстуры грани.
inline void faDrawSeg(uint32_t* nat, int NW, int NH, double* zc,
                      const Level& lvl, const Palette& wallPal, MetaCache& meta,
                      const Camera& cam, double x0, double y0, double x1, double y1,
                      uint16_t faceMeta, bool flipU, bool doShade, const ShadeRamps& ramps,
                      double uA = 0.0, double uB = 128.0,    // диапазон texU (для половинок створок двери)
                      bool cabinCol = false,    // true = стена самой кабины лифта (без питч-скролла)
                      int solidIdx = -1,        // >=0: СПЛОШНОЙ индекс палитры (синий floor-seg лифта/лестницы)
                      bool gradSeg = false,     // true: сегмент = ГРАДИЕНТ (FA_GRAD-маркер; wall-seg шахты)
                      bool overdraw = false,    // true: рисовать НА РАВНОЙ глубине (отлож. сегмент поверх стен)
                      bool deferFC = false,     // true: отлож. сегмент шахты — заливать ПОЛОСЫ потолок+пол (D5CE), не стену
                      uint16_t profWord = 0,    // (устар.) байт-профиль; заменён depth-наклоном (ниже)
                      int profBase = 0x40,      // (устар.)
                      bool shaftWall = false,   // true: СИНИЙ БЭКДРОП за стеной (ТОЛЬКО лифт)
                      double descD0 = 0.0,       // ГЛУБИНА СПУСКА у конца (x0,y0): позиция вдоль оси спуска отн. камеры
                      double descD1 = 0.0) {     // глубина спуска у конца (x1,y1). Наклон параллелограмма (однонапр.).
    double f0, l0, f1, l1;
    faProject(cam, x0, y0, f0, l0);
    faProject(cam, x1, y1, f1, l1);
    double u0 = flipU ? uB : uA, u1 = flipU ? uA : uB;
    double d0 = descD0, d1 = descD1;

    if (f0 < FA_NEAR && f1 < FA_NEAR) return;          // оба позади
    if (f0 < FA_NEAR) { double t = (FA_NEAR - f0) / (f1 - f0); l0 += t * (l1 - l0); u0 += t * (u1 - u0); d0 += t * (d1 - d0); f0 = FA_NEAR; }
    if (f1 < FA_NEAR) { double t = (FA_NEAR - f1) / (f0 - f1); l1 += t * (l0 - l1); u1 += t * (u0 - u1); d1 += t * (d0 - d1); f1 = FA_NEAR; }

    double sx0 = faFocalH() + faFocalH() * l0 / f0;
    double sx1 = faFocalH() + faFocalH() * l1 / f1;
    double iv0 = 1.0 / f0, iv1 = 1.0 / f1;
    if (sx0 > sx1) { std::swap(sx0, sx1); std::swap(iv0, iv1); std::swap(u0, u1); std::swap(d0, d1); }
    double span = sx1 - sx0;
    if (span < 1e-6) return;
    // перспективно-корректный texU: интерполируем u/forward и 1/forward, делим — иначе
    // аффинный (линейный по экрану) texU «плывёт» по близкой наклонной стене при вращении.
    double uz0 = u0 * iv0, uz1 = u1 * iv1;

    int ix0 = (int)std::ceil(sx0), ix1 = (int)std::floor(sx1);
    if (ix0 < 0) ix0 = 0; if (ix1 > NW - 1) ix1 = NW - 1;
    const uint8_t* mt = meta.get(faceMeta);  // 128x64
    const double yc = NH / 2.0;

    for (int ix = ix0; ix <= ix1; ++ix) {
        double t = (ix + 0.5 - sx0) / span;
        double iv = iv0 + t * (iv1 - iv0);   // 1/forward (линейно по экрану = перспект.-корректно)
        if (overdraw ? (iv < zc[ix]) : (iv <= zc[ix])) continue;  // overdraw: рисуем и на равной глубине
        double u = (uz0 + t * (uz1 - uz0)) / iv;   // перспективно-корректный texU
        // ТОЧНЫЙ скейлер ZT: D0 = ЦЕЛАЯ высота колонны (= scale = 0x10000/(forward>>6) = 64/forward).
        // Колонна = ровно D0 пикселей, центр на горизонте; тексель строки i = floor((i+0.5)*64/D0)
        // — бит-в-бит как скомпилированные рутины 0x4766..0x700c (центр-сэмплинг 64-стр. источника).
        int D0 = (int)(faFocalV() * iv);      // верт. фокус * iv (= 64/forward, макс ~80)
        if (D0 < 1) { zc[ix] = iv; continue; }
        int band = doShade ? bandForHeight(D0) : 0;
        int tmx = ((int)u) & 127;
        bool hiNib = !(tmx & 1);
        // СКОС ТЕКСТУРЫ СТЕНЫ НА СПУСКЕ (это РЕНДЕРИНГ ТЕКСТУРЫ, НЕ геометрия и НЕ высота камеры!): тексель-строка
        // сдвигается по колонне → текстура искажается по ДИАГОНАЛИ, но сама стена стоит на месте (прямоугольник).
        // Привязка к ГРАНИ (рампа (d1-d0)·t = наклон грани вдоль оси спуска) → СТАБИЛЬНО при движении (не «уплывает»,
        // не «перекашивает»). Только грани на склоне (d0≠d1) скошены; ровные (d0==d1) — обычная текстура.
        double t_col = (ix + 0.5 - sx0) / span;
        int texVShift = (int)((d1 - d0) * t_col * faStairK());   // диагональ текстуры (в текселях), привязана к грани
        (void)profWord; (void)profBase;
        // ЛЕСТНИЦА: питч-наклон колонны D0·pitch/64 (растеризатор ZT d6a8). ЛИФТ: стена САМОЙ КАБИНЫ
        // (cabinCol=true) — СТАТИЧНА (без питча); «пространство» коридора за выходом (cabinCol=false) едет
        // питчем вверх, снизу растёт синий пол/межэтажье (заливка фона уже синяя). Подробно см. renderFPS.
        const bool elevRide = (cam.elevState != 0);
        // ЛИФТ: стена КАБИНЫ (cabinCol) статична; стена КОРИДОРА (вид на выход) едет ПЕРСПЕКТИВНЫМ питчем
        // (D0·pitch/64) прямо в нативный буфер; статичный градиент-фон и межэтажный синий — в блите.
        const bool noPitch = cabinCol;
        // ПИТЧ: ЛИФТ — per-column D0·pitch/64 (как было, не ломаем). ЛЕСТНИЦА — РАВНОМЕРНЫЙ сдвиг горизонта
        // (Wolf3D y-shear, по фидбэку «нет наклона как в wolfenstein»): ВСЕ стены двигаются вместе → на уровне
        // друг друга, не «ныряют» (это и было «падает глубоко»). Bounded ±36.
        // ЛИФТ оставляем per-column (как было). ЛЕСТНИЦА/ПРЫЖОК/ПРИСЕД: ВЫСОТА камеры = сдвиг ВСЕГО вида целиком
        // (стены+пол+потолок) — делается в БЛИТЕ (viewShiftNat), а НЕ здесь (иначе двигались бы только стены =
        // ложный «наклон»/перекос). Тут стены лестницы НЕ сдвигаем питчем.
        // ВЫСОТА КАМЕРЫ = per-column eye-height. ЛИФТ — полный (D0·pitch/64, работает). ЛЕСТНИЦА — ОГРАНИЧЕН/масштаб
        // faStairUni: игра ограничивает высоту через ресемпл проекции D4A6 (в пределах 40-стр. половины + сжатие),
        // а безграничный per-column (до −D0≈−64) = «слишком сильный спуск». faStairUni<1 уменьшает силу спуска.
        int pshift = noPitch ? 0
                   : elevRide ? (int)(D0 * cam.pitch / 64.0)
                   : (int)(D0 * cam.pitch * faStairUni() / 64.0);
        // БЕЗ кламп-сжатия! Игра (D202: D3=(D0·pitch+проф)>>6) НЕ ограничивает сам сдвиг — она ОБРЕЗАЕТ колонну
        // по экрану (рисует только видимую часть, остальное за кадром). Кламп сжимал стену в полоску = баг.
        int top = (int)yc - D0 / 2 + pshift;  // ГЕОМЕТРИЯ стены БЕЗ скоса (скос — только в текстуре, ty ниже)
        // ОТЛОЖЕННЫЙ СЕГМЕНТ ШАХТЫ (D5CE): на колонку рисуем ПАРУ полос — потолок [0,top) + пол [top+D0,NH),
        // источник = синий idx1 (floor-seg) или ГРАДИЕНТ-маркер (wall-seg, FC «продолжается» сквозь стену).
        // Середина [top,top+D0) — НЕ трогаем (там настоящая стена кабины). Узкий x-диапазон клетки ограничивает.
        if (deferFC) {
            uint32_t src = (solidIdx >= 0) ? wallPal.c[solidIdx & 0x0F] : 0x00000001u;  // синий / FA_GRAD
            int botStart = top + D0;
            for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[y * NW + ix] = src;  // потолок
            for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[y * NW + ix] = src;  // пол
            // НЕ ставим zc[ix]: сегменты шахты не должны перекрывать друг друга (иначе ближний градиент-сег
            // с z-замком гасит дальний синий floor-seg → пропадал синий пол). Тест только против СТЕН.
            continue;
        }
        // СИНИЙ БЭКДРОП ШАХТЫ (лест/лифт-стена): позади стены — синее полотно idx1 (потолок над стеной + пол под
        // ней). Стена-текстура рисуется СПЕРХУ в [top,top+D0]; синее видно сверху/снизу = иллюзия пол/потолок шахты.
        if (shaftWall) {
            uint32_t blue = wallPal.c[1];                                  // VOID_BLUE idx1
            int botStart = top + D0;
            for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[y * NW + ix] = blue;  // потолок-задник
            for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[y * NW + ix] = blue;  // пол-задник
        }
        for (int i = 0; i < D0; ++i) {
            int y = top + i;
            if (y < 0 || y >= NH) continue;
            if (gradSeg)       { nat[y * NW + ix] = 0x00000001u; continue; }                 // градиент-сег (FA_GRAD→фон/пол-потолок в блите)
            if (solidIdx >= 0) { nat[y * NW + ix] = wallPal.c[solidIdx & 0x0F]; continue; }  // синий сегмент idx1
            int ty = (int)((i + 0.5) * 64.0 / D0) + texVShift;             // +скос: тексель-строка сдвинута (диагональ)
            if (ty < 0) ty = 0; else if (ty > 63) ty = 63;                 // клампим в 64-строчный источник
            uint8_t idx = mt[ty * 128 + tmx];
            if (idx == 0 && elevRide) continue;                       // лифт-кабина: оставляем маркер (синий в блите)
            // ОКНО (тексель-0 стены) = ВСЕГДА прозрачно (MD цвет 0) → виден Plane A (фон); маркер 0x00000000.
            // Стена непрозрачна → цвет. (Отличается от FA_GRAD=0x00000001 не-стены → пол/потолок.)
            nat[y * NW + ix] = (idx == 0) ? 0x00000000u : wallPal.c[ramps.map(idx, band, hiNib, doShade)];
        }
        zc[ix] = iv;
    }
}

// Полный кадр faithful. put(x,y,argb) в framebuffer W x H; zbuf[x]=глубина (для спрайтов).
template <typename PutFn>
void renderFaithful(PutFn&& put, int W, int H, const Level& lvl, const Palette& wallPal,
                    MetaCache& meta, const Camera& camIn, std::vector<double>& zbuf, int envMode,
                    int natW = FA_NW, int natH = -1) {
    // ВЫСОТА ГЛАЗА НА ЛЕСТНИЦЕ (дизасм BAEE: -71e6 -= subpos для класса≥6; D202: сдвиг колонны =(D0·pitch+
    // profile_acc)>>6, обрезка по экрану). АВТО ВЫКЛ: без реального профиль-аккумулятора (-6e84, компенсирует
    // питч и даёт V-скос) один питч уводит близкие стены блока за экран = всё синее. Питч+профиль связаны,
    // портировать вместе. Пока только тест-override (--stairpitch N); обычный рендер pitch=0 (рабочее состояние).
    Camera cam = camIn;
    cam.pitch -= (faStairPitchOverride() < 1e8) ? faStairPitchOverride()
                                                : stairPitchAdjust(lvl, cam.floor, cam.px, cam.py);
    const int NW = natW;                                          // ширина нат-буфера (фуллскрин 128, реф 256)
    const int NH = (natH > 0) ? natH : (int)((long)NW * H / W);   // высота: реф 80, иначе по пропорции окна
    faFocalH() = NW / 2.0;                                        // FOV 90° по ширине нат-буфера (реф 128, фуллскр 64)
    faFocalV() = 64.0;                                            // верт. фокус = 64 (ZT: D0=64/forward; под bandForHeight)
    const bool doShade = (envMode != 0);
    const bool openTop = (envMode == 3);
    const int horizon = NH / 2;
    static ShadeRamps ramps;            // CLUT-рампы затенения ZT (кэш по env)
    ramps.build(meta.shadeRampData, envMode);
    static FloorCeil fc;                // слой пол/потолок ZT (ROM-шаблон по env)
    fc.build(meta.fcTemplateData, wallPal, envMode);
    // КАРТА ВЫСОТ ПОЛА ЛЕСТНИЦЫ (кэш по этажу): пол клеток-спуска опущен → наклонный/смещённый вниз пол.
    static std::vector<int> stairHt; static int stairHtFloor = -1; static const Level* stairHtLvl = nullptr;
    if (stairHtFloor != cam.floor || stairHtLvl != &lvl) {
        buildStairHeightMap(lvl, cam.floor, stairHt); stairHtFloor = cam.floor; stairHtLvl = &lvl;
    }

    std::vector<uint32_t> nat((size_t)NW * NH);
    std::vector<double>  zc(NW, 0.0);

    // фон: потолок/пол (градиент к горизонту) или ПАНОРАМА (env3). ЛИФТ (transit): весь фон = SENTINEL,
    // разрешается в блите ПОКОЛОННО (фильмстрип-скролл «вида на выход» + синий межэтажный блок / синий
    // пол-потолок кабины) — НЕ заливаем всё синим тут (иначе пропадает градиент «пространства» этажа).
    const bool transit = (cam.elevState != 0);
    const Panorama* bg = activeBg();
    const bool hasBg = (bg && bg->valid());      // панорама доступна и в поездке (вид на выход env3)
    bgSkyTransp() = true;                          // тексель-0 стен ВСЕГДА прозрачен → Plane A (фон)
    // СЛОИСТАЯ МОДЕЛЬ ZT (по инфо игрока): Plane A = ФОН (панорама, универсальный backdrop за ВСЕМ);
    // Plane B = пол/потолок-градиент + стены. Где Plane B прозрачен → виден фон. Два маркера в nat:
    //   FA_GRAD (0x00000001) = НЕ-стена → пол/потолок-градиент (в блите; idx0 градиента = env3-небо → фон);
    //   FA_BG   (0x00000000) = ОКНО (тексель-0 стены) → фон НАПРЯМУЮ.
    // Стена непрозрачна (alpha=FF) → её цвет. Так фон просвечивает у окон/пустот/неба, а не «обрезанный градиент».
    const uint32_t FA_GRAD = 0x00000001u;
    for (size_t i = 0; i < nat.size(); ++i) nat[i] = FA_GRAD;
    (void)horizon;

    // обход клеток-стен в пределах ДАЛЬНОСТИ ПРОРИСОВКИ (cull @0xba60): box ±dist от камеры по X и Y.
    const int camCX = (int)cam.px, camCY = (int)cam.py;
    const int drawDist = (faDrawDist() > 0) ? faDrawDist() : drawDistForEnv(envMode);
    // КАМЕРА В ЛЕСТНИЧНОЙ СЕКЦИИ? Профиль-скос/питч/синий-бэкдроп лестницы рисуем ТОЛЬКО когда игрок ВНУТРИ
    // секции (стоит на клетке-полу 0x12-0x17). Иначе стены-лестницы (0x0c-0x11), видимые издалека через уровень,
    // рисовались бы скошенными + с синим бэкдропом (баг: «синий всегда виден», стены тонут). Вне секции — обычные стены.
    const uint8_t camCt = (camCX >= 0 && camCY >= 0 && camCX < Level::W && camCY < Level::H)
                          ? lvl.cellType(cam.floor, camCX, camCY) : 0;
    const bool camOnStair = isStairFloorCT(camCt) || isStairProfileCT(camCt);
    // ОСЬ СПУСКА (однонаправленный наклон): направление углубления пола секции. Глубина точки = проекция на ось
    // отн. камеры → наклон стен-параллелограммов консистентен (одна сторона для всех стен, симметрии нет).
    // НАКЛОН ТОЛЬКО НА СКЛОНЕ: ось углубления берём от КЛЕТКИ КАМЕРЫ (slope-клетка). На РОВНОМ участке (площадка
    // 0x13/0x15/0x16/0x17 → descentDeepDir пуст) наклона НЕТ (фикс «на ровной части перекашивает»).
    int ddx = 0, ddy = 0;
    descentDeepDir(camCt, ddx, ddy);
    const bool haveDescent = camOnStair && (ddx || ddy);
    // Глубина точки вдоль ОСИ СПУСКА (deeper = +). Грани ВДОЛЬ спуска имеют d0≠d1 → их ТЕКСТУРА скошена по диагонали.
    auto dD = [&](double wx, double wy) {
        return haveDescent ? ((wx - cam.px) * (double)ddx + (wy - cam.py) * (double)ddy) : 0.0;
    };
    for (int cy = 0; cy < Level::H; ++cy) {
        if (cy < camCY - drawDist || cy > camCY + drawDist) continue;
        for (int cx = 0; cx < Level::W; ++cx) {
            if (cx < camCX - drawDist || cx > camCX + drawDist) continue;
            uint8_t ct = lvl.cellType(cam.floor, cx, cy);
            if (!cellRenderWall(ct)) continue;
            uint8_t cell = lvl.cellId(cam.floor, cx, cy);
            auto openN = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= Level::W || ny >= Level::H) return false;
                uint8_t nct = lvl.cellType(cam.floor, nx, ny);
                // дверь занимает лишь центр клетки → её бока открыты: соседняя стена ДОЛЖНА
                // рисовать грань в сторону двери (иначе текстура стены у двери пропадает).
                return !cellRenderWall(nct) || cellIsDoor(nct);
            };
            auto metaOf = [&](int face) {
                uint16_t m = lvl.texorder(cell, face);
                if (m == 0 && cellIsDoor(ct)) m = DOOR_METATEX;
                return m;
            };

            // ДВЕРИ (celltype 6/7): панель в ЦЕНТРЕ ячейки (b036: addi #0x80 = полклетки), НЕ во всю
            // ячейку. ct6 гориз. — сегмент поперёк X на середине Y; ct7 верт. — поперёк Y на середине X.
            // Дверь проходима (cellBlocks её исключает) — сквозь неё ходим (как открытую).
            if (ct == 6 || ct == 7) {
                uint16_t m = metaOf(0);   // дверная метатекстура (fallback #18)
                // РАЗДВИЖНЫЕ СТВОРКИ: две половины уезжают к краям, ТЕКСТУРА ЕДЕТ С НИМИ (не маска).
                // Левая створка занимает [cx, cx+0.5−h] и показывает texU [h·128 .. 64] (часть текстуры
                // ушла в левую стену); правая [cx+0.5+h, cx+1] → texU [64 .. 128−h·128]. (Как в DDA.)
                double h = doorOpen(cam.floor, cx, cy) * 0.5;   // 0=закрыта, 0.5=полностью открыта
                double uLwall = h * 128.0, uRwall = 128.0 - h * 128.0;   // texU у стен (уехавшая часть)
                if (h < 0.499) {
                    if (ct == 6) {  // гориз.: левая [cx, cx+0.5−h], правая [cx+0.5+h, cx+1]
                        faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                                  cx, cy + 0.5, cx + 0.5 - h, cy + 0.5, m, false, doShade, ramps, uLwall, 64.0);
                        faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                                  cx + 0.5 + h, cy + 0.5, cx + 1, cy + 0.5, m, false, doShade, ramps, 64.0, uRwall);
                    } else {        // верт.
                        faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                                  cx + 0.5, cy, cx + 0.5, cy + 0.5 - h, m, false, doShade, ramps, uLwall, 64.0);
                        faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                                  cx + 0.5, cy + 0.5 + h, cx + 0.5, cy + 1, m, false, doShade, ramps, 64.0, uRwall);
                    }
                }
                continue;
            }

            // ДИАГОНАЛЬНЫЕ (угловые) стены: celltype 2..5 рисуются как диагональ через клетку
            // (по растеризатору ZT c842/c8a8/c910/c97c — проекция углов клетки на 1 ячейку):
            //   ct2(UR)/ct4(LL) → "\" (cx,cy)-(cx+1,cy+1), грань 3/0;
            //   ct3(LR)/ct5(UL) → "/" (cx,cy+1)-(cx+1,cy), грань 0/3.
            // Одна диагональ-сегмент вместо осевых рёбер; видна с обеих сторон.
            if (ct >= 2 && ct <= 5) {
                int face = (ct == 2 || ct == 5) ? 3 : 0;
                uint16_t m = metaOf(face);
                if (ct == 2 || ct == 4)   // "\"
                    faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                              cx, cy, cx + 1, cy + 1, m, false, doShade, ramps);
                else                       // "/"
                    faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                              cx, cy + 1, cx + 1, cy, m, false, doShade, ramps);
                continue;
            }

            // Лифт: стена самой кабины (elevator-клетка 0x30-0x5b) рисуется СТАТИЧНО (без питч-скролла);
            // коридор за выходом — обычная клетка → едет питчем (см. faDrawSeg / renderFPS).
            const bool isCabin = (cam.elevState != 0) && rcElevCell(ct);
            // ДИАГОНАЛЬ ЛЕСТНИЦЫ: профиль грани (L,R) для клеток-стен 0x0c-0x11. asc/desc по знаку под-позиции
            // камеры в клетке (-0x6e92 ≈ frac); пока приближаем asc (TODO: точный селектор по оси лестницы).
            const bool stairCell = isStairProfileCT(ct) && !faStairOff();   // скос рисуем КОНСИСТЕНТНО (без гейта = без «попа»)
            (void)camOnStair;
            // глубина спуска у конца грани (только для стен-лестниц; иначе 0 = без наклона)
            auto sD = [&](double wx, double wy) { return stairCell ? dD(wx, wy) : 0.0; };
            // North ребро (сосед сверху открыт) -> грань 0 (flipU=true, иначе зеркалит)
            if (openN(cx, cy - 1))
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy, cx + 1, cy, metaOf(0), true, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, false, sD(cx, cy), sD(cx + 1, cy));
            // South ребро -> грань 3 (flipU=false)
            if (openN(cx, cy + 1))
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy + 1, cx + 1, cy + 1, metaOf(3), false, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, false, sD(cx, cy + 1), sD(cx + 1, cy + 1));
            // West ребро (видно с востока, +X) -> грань 1 (flipU=false, иначе зеркалит)
            if (openN(cx - 1, cy))
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy, cx, cy + 1, metaOf(1), false, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, false, sD(cx, cy), sD(cx, cy + 1));
            // East ребро (видно с запада, -X) -> грань 2 (flipU=true)
            if (openN(cx + 1, cy))
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx + 1, cy, cx + 1, cy + 1, metaOf(2), true, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, false, sD(cx + 1, cy), sD(cx + 1, cy + 1));
        }
    }

    // ДЕКОР-БИЛБОРДЫ (faithful): в НАТИВНЫЙ буфер nat ДО блита (получат апскейл/растяжку/сдвиг как стены).
    // z-тест против zc[nx]=1/forward (больше=ближе); screenX по нат-фокусу (FOV 90°); высота клетки = 64/f.
    if (faDecor() && meta.obj && meta.obj->count > 0)
        drawDecorBillboards(NW, NH, lvl, cam, *meta.obj, wallPal, ramps, doShade, drawDist,
            [&](int x, int y, uint32_t c) { nat[(size_t)y * NW + x] = c; },
            [&](int x, double f) { return (1.0 / f) <= zc[x]; },
            [&](double f, double l) { return NW * 0.5 * (1.0 + l / f); },
            [&](double f) { return faFocalV() / f; });

    // ПОЛ ЛЕСТНИЦЫ/ШАХТЫ = FLOOR-CASTING (не барьер-полосы): синий наклонный пол клеток-спуска (0x12-0x14)
    // и пол/потолок-пустота клеток лифта рисуются в БЛИТЕ ниже (на каждый пиксель пола/потолка ищем клетку).
    // Старый отложенный проход полос (deferFC) убран — давал вертикальный «синий барьер» вместо пола.

    // апскейл нативного буфера в framebuffer + ГОРИЗОНТАЛЬНАЯ РАСТЯЖКА дисплея + z-буфер.
    // MD рендерит низкое верт. разрешение и растягивает по ширине → стены/двери ШИРОКИЕ.
    // Вертикаль оставляем 1:1 (полная), по горизонтали — растяжка вокруг центра (зум X).
    zbuf.assign(W, 1e9);
    const double hs = faHStretch();
    const double scX = (double)W / NW;            // базовый гор. масштаб
    const double cxN = NW / 2.0, cxF = W / 2.0;    // центр по X (натив / framebuffer)
    const int fbHorizon = H / 2;
    const uint32_t VOID_BLUE = 0xFF002448u;
    // межэтажный синий пол: спуск (cabin≥0) — наезжает СНИЗУ [blueLo..H]; после свопа (cabin<0) — уходит
    // СВЕРХУ [0..blueHi]; на пике |cabin|=0x40 — весь экран. Стена коридора уже сдвинута питчем в nat,
    // ГРАДИЕНТ-фон СТАТИЧЕН (screen-anchored, разрешается из sentinel по экранному y).
    int blueLo = 0, blueHi = 0;
    if (transit) { if (cam.cabin >= 0) { blueLo = (int)(H * (1.0 - cam.cabin / 64.0)); blueHi = H; }
                   else                { blueLo = 0; blueHi = (int)(H * (-cam.cabin / 64.0)); } }
    // ФОН (Plane A): универсальный backdrop-слой за всем. Панорама 1:1 (background.hpp bgSample),
    // позиционируется углом (hscroll) и этажом/кабиной (vscroll). Чёрный, если панорамы нет.
    auto bgAt = [&](int x_, int sy) -> uint32_t {
        return hasBg ? bgSample(*bg, cam.dirX, cam.dirY, cam.floor, cam.cabin, x_, W, sy, fbHorizon) : 0xFF000000u;
    };
    // ПОЛ/ПОТОЛОК-градиент (Plane B): точный дизер-паттерн ZT на нативной сетке (nx — нат.пиксель вида,
    // r — сканлайн-строка шаблона 0..79). 0x00000000 = прозрачно (idx0 = env3-небо).
    auto gradAt = [&](int x_, int sy) -> uint32_t {
        int nx = (int)(cxN + (x_ - cxF) / (scX * hs)); if (nx < 0) nx = 0; if (nx > NW - 1) nx = NW - 1;
        int ny = sy * NH / H;                          if (ny < 0) ny = 0; if (ny > NH - 1) ny = NH - 1;
        int r  = ny * 80 / NH;                         if (r > 79) r = 79;
        return floorCeilColorExact(wallPal, fc, nx, r);
    };
    // FLOOR-CASTING: пиксель пола (ny>hor) → forward-дистанция → мировая клетка. Клетки лестницы (пол ОПУЩЕН
    // по карте высот stairHt) и лифта → СИНИЙ. Высота h опускает пол: f = focalV·(0.5+h·HSTEP)/|ny−hor|.
    // Итерируем: оценили клетку при h=0, взяли её высоту, переоценили дистанцию (пол ниже → клетка дальше).
    // Луч столбца: lat = f·(nx−cxN)/cxN, perp = (−dirY, dirX). Так наклонный/смещённый пол спуска уходит вниз.
    // ПОЛ И ПОТОЛОК клеток лестницы (0x12-0x17) и лифта = СИНЯЯ ПУСТОТА (void) — плоский каст по лучу столбца.
    // Форму спуска создают СТЕНЫ (профиль: смещение вниз + скос), а синяя пустота заполняет вокруг них.
    // Если камера САМА в клетке-лестнице/лифта — ты в ШАХТЕ → весь пол/потолок-void синий (потолок не «отваливается»).
    // camInShaft (весь void синий) ТОЛЬКО для ЛИФТА — у лестницы синий идёт от БЭКДРОПА за стенами (shaftWall),
    // а не из заливки всего объёма (это и был «барьер»).
    // В ШАХТЕ (весь void синий) ТОЛЬКО ЛИФТ. Лестница — синий ПОЛ через флор-каст (ниже), не вся пустота.
    const bool camInShaft = (camCX >= 0 && camCY >= 0 && camCX < Level::W && camCY < Level::H) &&
                            rcElevCell(camCt);
    auto shaftFloorBlue = [&](int x_, int sy) -> bool {
        if (camInShaft) return true;                                 // в шахте лифта → весь void синий
        int nx = (int)(cxN + (x_ - cxF) / (scX * hs)); if (nx < 0) nx = 0; if (nx > NW - 1) nx = NW - 1;
        int ny = sy * NH / H;
        int dn = ny - NH / 2;
        if (dn == 0) return false;
        double f = (faFocalV() * 0.5) / (double)(dn < 0 ? -dn : dn);
        if (f < FA_NEAR || f > (double)drawDist + 2.0) return false;
        double lat = f * (nx - cxN) / cxN;
        int wcx = (int)(cam.px + cam.dirX * f - cam.dirY * lat);     // perp = (-dirY, dirX)
        int wcy = (int)(cam.py + cam.dirY * f + cam.dirX * lat);
        if (wcx < 0 || wcy < 0 || wcx >= Level::W || wcy >= Level::H) return false;
        uint8_t ct = lvl.cellType(cam.floor, wcx, wcy);
        if (rcElevCell(ct)) return true;                            // лифт: пол/потолок-void синий
        // ЛЕСТНИЦА: синий ПОЛ спуска (только низ, dn>0) там, где под полом клетка-лестница (флор-каст).
        if (camOnStair && dn > 0 && (isStairFloorCT(ct) || isStairProfileCT(ct))) return true;
        return false;
    };
    // ВЫСОТА КАМЕРЫ (pitch) = сдвиг ВСЕГО вида целиком (стены+пол+потолок двигаются вместе → нет наклона).
    // ОБЩИЙ механизм с прыжком/приседом (по дизасму -71e6 — высота камеры, easing ±4/кадр, сброс 0 при загрузке).
    // Лифт НЕ трогаем (свой per-column): viewShiftPx=0 при elevState. Источник берём со сдвигом, назначение — y.
    const int viewShiftPx = (cam.elevState != 0) ? 0 : (int)(cam.pitch * faStairUni());
    for (int x = 0; x < W; ++x) {
        int nx = (int)(cxN + (x - cxF) / (scX * hs));
        if (nx < 0) nx = 0; if (nx > NW - 1) nx = NW - 1;
        double iv = zc[nx];
        zbuf[x] = (iv > 0) ? 1.0 / iv : 1e9;
        // Лифт: колонна = «стена кабины» (rcElevCell → синий пол/потолок) или «вид на выход» (коридор).
        int mode = 0;                                 // 0 обычная, 1 коридор-лифт, 2 кабина-лифт
        if (transit) mode = rcElevCell(rcColumnHitCt(lvl, cam, x, W, hs)) ? 2 : 1;
        for (int y = 0; y < H; ++y) {
            if (transit && mode == 1 && y >= blueLo && y < blueHi) { put(x, y, VOID_BLUE); continue; } // межэтажный синий
            int sy = y - viewShiftPx;                  // сдвиг ВСЕГО вида по высоте камеры (источник)
            if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;       // край вытягивается (без чёрных полос)
            uint32_t v = nat[(size_t)(sy * NH / H) * NW + nx];
            uint32_t c;
            if ((v & 0xFF000000u) != 0)          c = v;            // непрозрачная стена (Plane B)
            else if (transit && mode == 2)       c = VOID_BLUE;    // кабина лифта → синий
            else if (v == FA_GRAD) {                               // не-стена → пол/потолок-градиент
                if (shaftFloorBlue(x, sy))       c = VOID_BLUE;    // пол/потолок клетки-шахты/спуска → синий
                else {
                    c = gradAt(x, sy);
                    if ((c & 0xFF000000u) == 0) c = bgAt(x, sy);   // idx0 (env3-небо) → ФОН (Plane A)
                }
            } else                               c = bgAt(x, sy);  // FA_BG: окно (тексель-0) → ФОН
            put(x, y, c);
        }
    }
}
