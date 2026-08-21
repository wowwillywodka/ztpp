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
#include "sniper_overlay.hpp"   // фоновый снайпер эп2 (оверлей за стенами + рикошеты)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

// ОТЛАДКА: путь для дампа НАТИВНОГО индекс-буфера стен (nat=hi / natLo=lo палитр-индексы 0..15,
// -1=не-стена/маркер). Для почисленной сверки рендера стен с оригиналом ZT (framebuffer numbers).
inline const char*& faDumpIdxPath() { static const char* p = nullptr; return p; }

// Параметры проекции ZT (из ccb6 / sin-cos @0x8124, M=256).
// Высота колонки = scale = 0x10000/(forward>>6); по геометрии стена 1×1 клетка рендерится
// КВАДРАТНО (height==width). Прямоугольный вид в игре = вертикальная растяжка дисплея MD.
static const int    FA_NW = 128;       // ширина нат-буфера по умолчанию (фуллскрин)
static const double FA_FOCAL = 64.0;   // дефолтный фокус
static const double FA_NEAR = 0.02;
// СИНИЙ ПОЛ/ПОТОЛОК ЛИФТА = idx1 палитры ep1 (ROM 0xd074 = 80×0x11; CRAM 0x0420 = RGB(0,36,72)). §9.3.
// ⚠ ep1-специфично: в др. эпизодах idx1 иной (красный/зелёный). TODO: брать из палитры пол/потолок эпизода.
static const uint32_t FA_ELEV_BLUE = 0xFF002448u;

// Раздельные ФОКУСЫ (нативный рендер ZT = 256×80, НЕквадратный пиксель 2:1): горизонтальный фокус
// = natW/2 (FOV 90° по ширине), вертикальный = 64 (высота колонны D0=64/forward, макс 80 = вся высота
// вида). Для фуллскрина natW=128→focalH=64; для РЕФЕРЕНСА natW=256→focalH=128, natH=80. Ставятся в
// renderFaithful, читаются в faDrawSeg (как faHStretch).
inline double& faFocalH() { static double v = 64.0; return v; }   // гор. фокус (screenX)
inline double& faFocalV() { static double v = 64.0; return v; }   // верт. фокус (высота D0)
// faDrawDist() — в raycaster.hpp (нужен и renderFPS, который объявлен раньше).

// ── FIXED-POINT ПРОЕКЦИЯ (fxProj, этап 1) — арифметика МД ca4c на ЦЕЛЫХ. Граница компромисса этапа 1:
// perp/доля грани приходят из double-марша (веер целым станет на этапе 3, fxfan), но КВАНТУЮТСЯ здесь к
// ROM-сеткам (усечение, не round — как divs/asr МД), дальше только int. Предполагает faFocalV=64 (ROM).
inline int fxD0FromPerp(double perp) {            // D0 = divs.w(0x10000, forward>>6) ≈ 64/forward (ca4c)
    int32_t f6 = (int32_t)(perp * 65536.0) >> 6;  // forward кл → 16.16 (=8.8 поз × 8.8 cos у МД) → >>6
    if (f6 <= 0) return 0x7FFF;                   // в упор (forward<64 суб-ед) → кламп как bvs-пропуск
    int32_t q = 0x10000 / f6;                     // divs.w: усечение к нулю
    return q > 0x7FFF ? 0x7FFF : (int)q;
}
inline int fxTexel(double t) {                    // доля грани 0..1 → тексель 0..127: суб-поз 8.8, тексель=sub>>1
    return (((int)(t * 256.0)) & 255) >> 1;       // (128 текселей на 256 суб-единиц клетки)
}

// faHStretch(), ShadeRamps, bandForHeight — общие, объявлены в raycaster.hpp.
// Освещение faithful = ТОЧНАЯ таблица ZT height→band (bandForHeight) + рампы (ShadeRamps) с
// дизером hi/lo. Bright(env0) — без рампы. Никаких аппроксимаций/Bayer.

// ПОЛОСА ТЕКСТУРЫ + WITHIN-BYTE ДИЗЕР VDP (4bpp: 1 байт = 2 гориз.пикселя). ДИЗАСМ ccb6/скейлеры 0x578e:
// каждая 2px-колонка = ПОЛОСА тайла (column-major, байт=строка полосы: hi-ниббл=ЛЕВЫЙ тексель, lo=ПРАВЫЙ),
// затенённая ПОЛНОЙ CLUT[band][(texL<<4)|texR] (move.b (a3,d0.w),(a0); addq #4,a0 — прямой табличный вывод БЕЗ
// инверсии фазы по строке/колонке). Выход: hi-ниббл=левый px, lo=правый, ПОСТОЯННО вниз ⇒ дизер = ВЕРТ.ПОЛОСЫ.
// texL/texR = ДВА РАЗНЫХ соседних текселя (метатекстура 128-шир, полоса = tmx&~1, +1) → полное гориз.разрешение
// как МД (без этого порт брал 1 тексель на обе половины = вдвое грубее, «мыльнее» + сильнее «уплывание»).
// Прозрачность (окно) — по ИСХОДНОМУ текселю 0, ПОСУБПИКСЕЛЬНО. faWallLo() = буфер правого px (блит: nat→чёт x).
inline uint32_t*& faWallLo() { static uint32_t* p = nullptr; return p; }
inline void faWallStrip(uint32_t* nat, int off, const uint8_t* mt, int row, int tmx,
                        const Palette& wallPal, const ShadeRamps& ramps, int band, bool doShade, bool elevRide,
                        bool clutDither) {   // clutDither: полный CLUT[band][byte] (дизер) — ТОЛЬКО дальние (D0≤0x50), см. faDrawSeg
    uint32_t* natLo = faWallLo();
    uint8_t texL, texR;
    if (faStripTexel()) { int sL = tmx & 0x7E; texL = mt[row + sL]; texR = mt[row + sL + 1]; }  // полоса = 2 текселя
    else { texL = texR = mt[row + (tmx & 0x7F)]; }                                              // старый путь: 1 тексель
    uint8_t lpx, rpx;
    // ⭐ROM-путь (сверено с MAME VRAM e1l1): затенение ДАЛЬНИХ стен (D0≤0x50, скейлер-путь ccb6 `move.b
    // (a3,d0.w),(a0)`) = ПОЛНАЯ CLUT full[band][(texL<<4)|texR], hi=лев.px/lo=прав.px. CLUT 0x3392 САМ дизерит
    // диагональ (band1 тексель2→0x21 …) → однородная стена = ДИЗЕР-байт = ВЕРТ.ПОЛОСЫ вдаль (33%≈ориг.39%).
    // БЛИЗКИЕ стены (D0>0x50, путь ccb6 ce74 = СЫРОЙ тексель БЕЗ CLUT-дизера) → диагональ hi[band][t] (полос НЕТ,
    // бит-точно как прежде). clutDither ставит faDrawSeg по D0 — иначе мой дизер ломал близкие стены (фидбэк).
    if (doShade) {
        if (clutDither) { uint8_t o = ramps.full[band][((int)texL << 4) | texR]; lpx = o >> 4; rpx = o & 0x0F; }
        else { lpx = ramps.hi[band][texL & 0x0F]; rpx = ramps.hi[band][texR & 0x0F]; }
    }
    else { lpx = texL; rpx = texR; }                    // Bright: сырые текселя
    if (texL == 0) { if (!elevRide) nat[off] = 0x00000000u; }   // окно/маркер кабины: прозрачно (elev — не трогаем)
    else nat[off] = wallPal.c[lpx];
    if (natLo) { if (texR == 0) { if (!elevRide) natLo[off] = 0x00000000u; }
                 else natLo[off] = wallPal.c[rpx]; }
}

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
    // ПРОФИЛЬ-грань лестницы (descD0/D1 = L,R сдвиги вершин). Инвариант к swap концов. Для профиль-грани
    // используем double-путь текстуры (fx-путь strip-texel рассогласуется с профиль-сдвигом → «хаос текстур»).
    const bool profileFace = faElevZT() && (d0 != 0.0 || d1 != 0.0);
    // ROM d0c4/ccb6: профиль L (=descD0) применяется на конце texU=0 (P1), R на texU=255. Порт кладёт L на
    // геом. x0, но при flipU (грани N/E) texU=0 на x1 (u1=uA=0) → L/R инвертированы → шир в НЕВЕРНУЮ сторону.
    // Выравниваем L к texU=0 концу. flipU==false (S/W): x0=texU0, уже верно.
    if (profileFace && flipU && faStairFaceLR()) std::swap(d0, d1);

    bool clipped = false;
    if (faFrustumClip()) {
        // ROM ca4c: клип к 90°-ФРУСТУМУ. near (f>0) → левая (l+f>=0, screenX 0) → правая (f−l>=0, screenX NW).
        // f/l/u/d интерполируются ЛИНЕЙНО ВДОЛЬ РЕБРА (мир) → texU в точке клипа перспективно-верен, внутри аффин.
        if (f0 < FA_NEAR && f1 < FA_NEAR) return;
        if (f0 < FA_NEAR) { double t=(FA_NEAR-f0)/(f1-f0); l0+=t*(l1-l0); u0+=t*(u1-u0); d0+=t*(d1-d0); f0=FA_NEAR; clipped=true; }
        if (f1 < FA_NEAR) { double t=(FA_NEAR-f1)/(f0-f1); l1+=t*(l0-l1); u1+=t*(u0-u1); d1+=t*(d0-d1); f1=FA_NEAR; clipped=true; }
        double a0 = l0 + f0, a1 = l1 + f1;             // левая плоскость: внутри = l+f>=0
        if (a0 < 0 && a1 < 0) return;
        if (a0 < 0)      { double t=a0/(a0-a1); double nf=f0+t*(f1-f0),nl=l0+t*(l1-l0),nu=u0+t*(u1-u0),nd=d0+t*(d1-d0); f0=nf;l0=nl;u0=nu;d0=nd; clipped=true; }
        else if (a1 < 0) { double t=a0/(a0-a1); double nf=f0+t*(f1-f0),nl=l0+t*(l1-l0),nu=u0+t*(u1-u0),nd=d0+t*(d1-d0); f1=nf;l1=nl;u1=nu;d1=nd; clipped=true; }
        double b0 = f0 - l0, b1 = f1 - l1;             // правая плоскость: внутри = f−l>=0
        if (b0 < 0 && b1 < 0) return;
        if (b0 < 0)      { double t=b0/(b0-b1); double nf=f0+t*(f1-f0),nl=l0+t*(l1-l0),nu=u0+t*(u1-u0),nd=d0+t*(d1-d0); f0=nf;l0=nl;u0=nu;d0=nd; clipped=true; }
        else if (b1 < 0) { double t=b0/(b0-b1); double nf=f0+t*(f1-f0),nl=l0+t*(l1-l0),nu=u0+t*(u1-u0),nd=d0+t*(d1-d0); f1=nf;l1=nl;u1=nu;d1=nd; clipped=true; }
    } else {
        if (f0 < FA_NEAR && f1 < FA_NEAR) return;          // оба позади (прежний near-plane clip)
        if (f0 < FA_NEAR) { double t = (FA_NEAR - f0) / (f1 - f0); l0 += t * (l1 - l0); u0 += t * (u1 - u0); d0 += t * (d1 - d0); f0 = FA_NEAR; }
        if (f1 < FA_NEAR) { double t = (FA_NEAR - f1) / (f0 - f1); l1 += t * (l0 - l1); u1 += t * (u0 - u1); d1 += t * (d0 - d1); f1 = FA_NEAR; }
    }

    double sx0 = faFocalH() + faFocalH() * l0 / f0;
    double sx1 = faFocalH() + faFocalH() * l1 / f1;
    double iv0 = 1.0 / f0, iv1 = 1.0 / f1;
    // ЦЕЛОЧИСЛЕННАЯ ПРОЕКЦИЯ как МД (ca4c, 68000 БЕЗ FPU): cos/sin ×256, forward/lateral 32-бит целые,
    // screenX = lateral/(forward>>6)+64, D0 = 0x10000/(forward>>6) — целочисленным делением (усечение как divs.w).
    // Числа «прилипают к сетке» → текстура не плывёт суб-пиксельно. Не для граней, пересекающих камеру (near-cross).
    bool fx = faFixedPoint() && !clipped && f0 > FA_NEAR && f1 > FA_NEAR && (!profileFace || faStairFixedTex());  // профиль: float, ЛИБО fixed при faStairFixedTex (ROM ccb6-путь)
    int fD0_0 = 0, fD0_1 = 0;
    if (fx) {
        // cos/sin — РЕАЛЬНЫЙ LUT ROM 0x8124 (не round(cos·256): отличается на ±1). Индекс = квант.угол+128 (МД a0=−Y).
        int32_t cs, sn;
        if (zAngLUTok()) { int idx = ((int)std::lround(camDirToAng512(cam)) + 128) & 511; cs = zAngLUT()[idx*2]; sn = zAngLUT()[idx*2+1]; }
        else { cs = (int32_t)std::lround(cam.dirX*256.0); sn = (int32_t)std::lround(cam.dirY*256.0); }
        int32_t cX = (int32_t)std::lround(cam.px*256.0), cY = (int32_t)std::lround(cam.py*256.0);  // камера 8.8
        int32_t X0 = (int32_t)std::lround(x0*256.0), Y0 = (int32_t)std::lround(y0*256.0);          // углы грани 8.8
        int32_t X1 = (int32_t)std::lround(x1*256.0), Y1 = (int32_t)std::lround(y1*256.0);
        // forward/lateral: muls.w + 32-бит сложение, усечение до int32 (wrap как 68k) — c9a8
        int32_t F0 = (int32_t)((int64_t)(X0-cX)*cs + (int64_t)(Y0-cY)*sn);
        int32_t L0 = (int32_t)((int64_t)(Y0-cY)*cs - (int64_t)(X0-cX)*sn);
        int32_t F1 = (int32_t)((int64_t)(X1-cX)*cs + (int64_t)(Y1-cY)*sn);
        int32_t L1 = (int32_t)((int64_t)(Y1-cY)*cs - (int64_t)(X1-cX)*sn);
        int32_t f6a = F0>>6, f6b = F1>>6;
        if (f6a > 0 && f6b > 0) {
            // ca4c: divs.w — 16-бит частное, усечение к нулю (как int/ в C++). Переполнение >0x7FFF → у МД bvs (пропуск); клампим.
            auto divw = [](int32_t num, int32_t den){ int32_t q = num/den; return q>0x7FFF?0x7FFF:(q<-0x8000?-0x8000:q); };
            sx0 = (double)(divw(L0, f6a) + 64); sx1 = (double)(divw(L1, f6b) + 64);        // screenX целое
            fD0_0 = divw(0x10000, f6a); fD0_1 = divw(0x10000, f6b);                        // D0 целое
            iv0 = fD0_0 / faFocalV(); iv1 = fD0_1 / faFocalV();
        } else fx = false;                                                                 // грань за камерой → float-путь
    }
    if (sx0 > sx1) { std::swap(sx0, sx1); std::swap(iv0, iv1); std::swap(u0, u1); std::swap(d0, d1); std::swap(fD0_0, fD0_1); }
    double span = sx1 - sx0;
    if (span < 1e-6) return;
    // texU: ROM ccb6 — АФФИННЫЙ (линейный по экрану, шаг Δu·256/ncols на колонку, БЕЗ деления на глубину):
    // текстура «плывёт» на близких наклонных стенах — так в оригинале (reference). faAffineTexU() OFF =
    // перспективно-корректный (интерполируем u/forward и 1/forward, делим) — гладко, но не как оригинал.
    const bool affU = faAffineTexU();
    double uz0 = u0 * iv0, uz1 = u1 * iv1;   // для перспективного пути

    int ix0 = (int)std::ceil(sx0), ix1 = (int)std::floor(sx1);
    if (ix0 < 0) ix0 = 0; if (ix1 > NW - 1) ix1 = NW - 1;
    const uint8_t* mt = meta.get(faceMeta);  // 128x64
    const double yc = NH / 2.0;
    // Целочисл. шаги texU (.8) и высоты (<<6) на колонку — как ccb6 (cd76: (Δu·256)/ncols; cd8c: (Δh<<6)/ncols).
    long fu0 = 0, fustep = 0, fdstep = 0; int fsx0i = 0, fd0hi = 0;
    if (fx) {
        // ⛔ faEdgeClamp = ДЕД-ЭНД (дефолт OFF): кламп screenX + сжатие текстуры у края — оригинал (MAME) СОВПАЛ
        // с CLIP (OFF), не с этим. Держать OFF. Ветка оставлена только для A/B. См. tuning.hpp / dead_ends.md.
        int sxL = (int)sx0, sxR = (int)sx1;
        if (faEdgeClamp()) { if (sxL < 0) sxL = 0; if (sxR > NW - 1) sxR = NW - 1; }
        int ncols = sxR - sxL + 1; if (ncols < 1) ncols = 1;
        fu0 = (long)std::lround(u0) << 8; fd0hi = fD0_0; fsx0i = sxL;
        fustep = (((long)std::lround(u1 - u0)) << 8) / ncols;
        fdstep = (((long)(fD0_1 - fD0_0)) << 6) / ncols;
    }
    // ПРОФИЛЬ-СКОС ЛЕСТНИЦЫ (ROM d214/d0c4): d0,d1 (=descD0/D1) = знаковые сдвиги вершин концов грани
    // (0x40=+64..); 0=обычная грань. profAccum интерполирует L·D0(лев)→R·D0(прав) → сдвиг ГЕОМЕТРИИ колонны
    // (D0·pitch + profAccum)/64. Над/под скошенной гранью = сплошной синий 0xd074 (§9.2). Только faElevZT.
    const double profL = d0 * faFocalV() * iv0;   // L·D0(левый конец)
    const double profR = d1 * faFocalV() * iv1;   // R·D0(правый конец)

    for (int ix = ix0; ix <= ix1; ++ix) {
        double iv, u; int D0;
        if (fx) {                                          // ЦЕЛОЧИСЛ. как МД: texU/высота дискретными шагами (не суб-пиксель)
            int col = ix - fsx0i;
            D0 = (int)((((long)fd0hi << 6) + fdstep * col) >> 6);          // высота линейно по экрану (целая)
            iv = D0 / faFocalV();
            if (affU) u = (double)(int)(((fu0 + fustep * col) >> 8) & 127); // АФФИННЫЙ целый тексель (ccb6, но полосит на грейзинге)
            else { double t = (ix + 0.5 - sx0) / span; double ivp = iv0 + t * (iv1 - iv0); // ПЕРСПЕКТИВА: u/z /(1/z) — без полос-недосэмплинга
                   u = (ivp > 1e-9) ? ((uz0 + t * (uz1 - uz0)) / ivp) : (u0 + t * (u1 - u0)); }
        } else {
            double t = (ix + 0.5 - sx0) / span;
            iv = iv0 + t * (iv1 - iv0);                     // 1/forward (перспект.-корректно)
            u  = affU ? (u0 + t * (u1 - u0)) : ((uz0 + t * (uz1 - uz0)) / iv);  // аффинный / перспективный texU
            D0 = (int)(faFocalV() * iv);                   // ТОЧНЫЙ скейлер: D0 = 64/forward (тексель floor((i+0.5)*64/D0))
        }
        if (overdraw ? (iv < zc[ix]) : (iv <= zc[ix])) continue;  // overdraw: рисуем и на равной глубине
        if (D0 < 1) { zc[ix] = iv; continue; }
        // БАНД CLUT: ROM грузит -$7176 (0x3392) фиксированно, БЕЗ дистанц.смещения (grep: 2 CLUT-скейлера,
        // оба band 0). faWallBand0()=стены всегда band 0 (как ROM, меньше дизера вдаль). OFF=дистанц.банды (было).
        bool shadeCol = doShade;
        int band = 0;
        if (doShade) {
            if (faJuneBands()) { int jb = juneBandForHeight(D0); if (jb < 0) shadeCol = false; else band = jb; }
            else band = faWallBand0() ? 0 : bandForHeight(D0);
        }
        // ⭐ФОНАРЬ [cf36/d138]: конус 16 колонн вокруг центра → таблица скейлеров 0x4436 (без CLUT) = затенение ВЫКЛ.
        if (flActive() && ix >= NW / 2 - NW / 16 && ix < NW / 2 + NW / 16) shadeCol = false;
        // CLUT-ДИЗЕР (верт.полосы) — ТОЛЬКО дальние стены (D0 ≤ faWallDitherD0, дефолт 0x28=40). БЛИЗКИЕ (D0 больше)
        // = СЫРАЯ (диагональ, без дизера, бит-точно). Порог откалиброван по MAME (свип к стене: оригинал сырой до
        // D0≈46, дизер от D0≈39), НЕ 0x50 (то — ce74-скейлер, не переключатель дизера). См. tuning.hpp.
        bool clutDither = faWallDither() && (D0 <= faWallDitherD0());
        int tmx = ((int)u) & 127;
        // ДИЗЕР+ПОЛОСА: within-byte (см. faWallStrip) — колонка = полоса 2 текселей, полная CLUT[band][byte],
        // hi-ниббл=левый px / lo=правый, БЕЗ флипа фазы (дизасм скейлеров 0x578e). => ВЕРТИКАЛЬНЫЕ ПОЛОСЫ.
        // СКОС ТЕКСТУРЫ СТЕНЫ НА СПУСКЕ (это РЕНДЕРИНГ ТЕКСТУРЫ, НЕ геометрия и НЕ высота камеры!): тексель-строка
        // сдвигается по колонне → текстура искажается по ДИАГОНАЛИ, но сама стена стоит на месте (прямоугольник).
        // Привязка к ГРАНИ (рампа (d1-d0)·t = наклон грани вдоль оси спуска) → СТАБИЛЬНО при движении (не «уплывает»,
        // не «перекашивает»). Только грани на склоне (d0≠d1) скошены; ровные (d0==d1) — обычная текстура.
        double t_col = (ix + 0.5 - sx0) / span;
        // ПРОФИЛЬ-грань лестницы (ROM d214): скос = сдвиг ГЕОМЕТРИИ колонны (в pshift), НЕ тексель-строки.
        // legacy (не profileFace): texVShift = скос тексель-строки (реконструкция faStairK).
        int texVShift = profileFace ? 0 : (int)((d1 - d0) * t_col * faStairK());
        double profAccum = profileFace ? (profL + (profR - profL) * t_col) : 0.0;   // L·D0лев → R·D0прав
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
        // ROM-ТОЧНЫЙ путь (faTransitZT): d214 сдвигает КАЖДУЮ колонну на D0·pitch/64 полной силы —
        // видимую «слишком сильную» просадку компенсирует НЕ ослабление сдвига, а D4A6-ресемпл фона
        // (fcRowD4A6) + обрезка колонны по экрану (что у нас и так есть). Иначе — прежние компромиссы.
        const double stairPitchSign = (profileFace && faStairFlip()) ? -1.0 : 1.0;  // A/B знак питча лестницы (баг №1)
        int pshift = noPitch ? 0
                   : profileFace ? (int)((D0 * cam.pitch * stairPitchSign + profAccum) / 64.0)   // d214: (D0·pitch + профиль)>>6 = скошенный квад
                   : faTransitZT() ? (int)(D0 * cam.pitch / 64.0)
                   : elevRide ? (int)(D0 * cam.pitch / 64.0)
                   : (int)(D0 * cam.pitch * faStairUni() / 64.0);
        // БЕЗ кламп-сжатия! Игра (D202: D3=(D0·pitch+проф)>>6) НЕ ограничивает сам сдвиг — она ОБРЕЗАЕТ колонну
        // по экрану (рисует только видимую часть, остальное за кадром). Кламп сжимал стену в полоску = баг.
        const int Dh = D0 * faWallHMul();     // ⭐June: экранная высота колонны = 2×D0 (ZT: D0) [c812]
        int top = (int)yc - Dh / 2 + pshift;  // ГЕОМЕТРИЯ стены БЕЗ скоса (скос — только в текстуре, ty ниже)
        // ОТЛОЖЕННЫЙ СЕГМЕНТ ШАХТЫ (D5CE): на колонку рисуем ПАРУ полос — потолок [0,top) + пол [top+D0,NH),
        // источник = синий idx1 (floor-seg) или ГРАДИЕНТ-маркер (wall-seg, FC «продолжается» сквозь стену).
        // Середина [top,top+D0) — НЕ трогаем (там настоящая стена кабины). Узкий x-диапазон клетки ограничивает.
        if (deferFC) {
            uint32_t src = (solidIdx >= 0) ? FA_ELEV_BLUE : 0x00000001u;  // синий idx1 (area-пол) / FA_GRAD (кабина-градиент)
            int botStart = top + Dh;
            for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[y * NW + ix] = src;  // потолок
            for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[y * NW + ix] = src;  // пол
            // НЕ ставим zc[ix]: сегменты шахты не должны перекрывать друг друга (иначе ближний градиент-сег
            // с z-замком гасит дальний синий floor-seg → пропадал синий пол). Тест только против СТЕН.
            continue;
        }
        // СИНИЙ БЭКДРОП ШАХТЫ (лест/лифт-стена): позади стены — синее полотно idx1 (потолок над стеной + пол под
        // ней). Стена-текстура рисуется СПЕРХУ в [top,top+D0]; синее видно сверху/снизу = иллюзия пол/потолок шахты.
        // Синий над/под: ШАХТА лифта (shaftWall) ИЛИ скошенная ПРОФИЛЬ-грань лестницы (§9.2, путь d0c4→d11a=0xd074).
        if (shaftWall || profileFace) {
            uint32_t blue = FA_ELEV_BLUE;                                  // idx1 синий (пол/потолок лифта/проём лестницы)
            int botStart = top + Dh;
            for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[y * NW + ix] = blue;  // потолок-задник
            for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[y * NW + ix] = blue;  // пол-задник
        }
        // СВЕРХВЫСОКАЯ КОЛОННА (ROM cfb2/ce74, D0>0x50 ≈ ближе ~0.8 клетки): вместо цикла на всю D0 — ровно NH
        // строк из ЦЕНТРА наружу с Bresenham-шагом текселя (d3−=0x40; при <0 след.тексель, d3+=D0); верхний
        // тайл идёт вверх (тексели 31..0), нижний вниз (32..63). Точное ROM-округление, без лишних итераций.
        if (Dh > 0x50 && solidIdx < 0 && !gradSeg && pshift == 0) {   // ТОЛЬКО без питча: при прыжке/приседе (pshift≠0)
            // центр из NH строк сместился бы и ОБРЕЗАЛ колонну сверху — тогда падаем в общий цикл ниже (тянет корректно).
            int cy0 = (int)yc;                                // центр колонны (питча нет)
            int upTex = 31, loTex = 32, d3 = Dh;
            for (int n = 0; n < NH / 2; ++n) {
                int yu = cy0 - (n + 1), yd = cy0 + n;          // строка вверх (верх.тексель) / вниз (ниж.тексель)
                if (yu >= 0 && yu < NH) {
                    int ty = upTex + texVShift; if (ty < 0) ty = 0; else if (ty > 63) ty = 63;
                    faWallStrip(nat, yu * NW + ix, mt, ty * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutDither);
                }
                if (yd >= 0 && yd < NH) {
                    int ty = loTex + texVShift; if (ty < 0) ty = 0; else if (ty > 63) ty = 63;
                    faWallStrip(nat, yd * NW + ix, mt, ty * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutDither);
                }
                d3 -= 0x40;
                if (d3 < 0) { if (upTex > 0) --upTex; if (loTex < 63) ++loTex; d3 += Dh; }
            }
            zc[ix] = iv;
            continue;
        }
        for (int i = 0; i < Dh; ++i) {
            int y = top + i;
            if (y < 0 || y >= NH) continue;
            if (gradSeg)       { nat[y * NW + ix] = 0x00000001u; continue; }                 // градиент-сег (FA_GRAD→фон/пол-потолок в блите)
            if (solidIdx >= 0) { nat[y * NW + ix] = FA_ELEV_BLUE; continue; }  // синий сегмент idx1 (пол лифта/лестницы)
            int ty = (int)((i + 0.5) * 64.0 / Dh) + texVShift;             // +скос: тексель-строка сдвинута (диагональ)
            if (ty < 0) ty = 0; else if (ty > 63) ty = 63;                 // клампим в 64-строчный источник
            // ПОЛОСА: 2 текселя (tmx&~1,+1) + полная CLUT; окно (тексель-0) прозрачно посубпиксельно; elevRide=маркер кабины.
            faWallStrip(nat, y * NW + ix, mt, ty * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutDither);
        }
        zc[ix] = iv;
    }
}

// Полный кадр faithful. put(x,y,argb) в framebuffer W x H; zbuf[x]=глубина (для спрайтов).
template <typename PutFn>
void renderFaithful(PutFn&& put, int W, int H, const Level& lvl, const Palette& wallPal,
                    MetaCache& meta, const Camera& camIn, std::vector<double>& zbuf, int envMode,
                    int natW = FA_NW, int natH = -1) {
    // ⭐НОЧНИК (ROM 11286): градиент пола/потолка = ИСТОЧНИК BRIGHT 0x14EE66 (тьма этажа отменяется — «видно
    // в темноте»), env3 (no-ceiling) сохраняет панораму. Палитра уже зелёная (scenePal 0x2072, ROM 29ee).
    if (nvActive() && envMode != 3) envMode = 0;
    // ВЫСОТА ГЛАЗА НА ЛЕСТНИЦЕ (дизасм BAEE: -71e6 -= subpos для класса≥6; D202: сдвиг колонны =(D0·pitch+
    // profile_acc)>>6, обрезка по экрану). АВТО ВЫКЛ: без реального профиль-аккумулятора (-6e84, компенсирует
    // питч и даёт V-скос) один питч уводит близкие стены блока за экран = всё синее. Питч+профиль связаны,
    // портировать вместе. Пока только тест-override (--stairpitch N); обычный рендер pitch=0 (рабочее состояние).
    Camera cam = camIn;
    // ROM b9a6: глаз рендера = позиция игрока − (dir)/8 (1/8 клетки назад, «за плечом») — безусловно в игре
    if (faCamBack()) {
        if (zAngLUTok() && cam.angI >= 0) {                     // ЦЕЛОЧИСЛ. как МД (b9a6): camX −= cos>>3, camY −= sin>>3 (8.8, asr=floor)
            int cs = zAngLUT()[cam.angI * 2], sn = zAngLUT()[cam.angI * 2 + 1];
            int ex = (int)std::lround(cam.px * 256.0) - (cs >> 3);
            int ey = (int)std::lround(cam.py * 256.0) - (sn >> 3);
            cam.px = ex / 256.0; cam.py = ey / 256.0;
        } else { cam.px -= cam.dirX * 0.125; cam.py -= cam.dirY * 0.125; }
    }
    // ЛЕСТНИЦА-ПИТЧ: при faElevZT cam.pitch УЖЕ = −cabin (rcUpdateTransitZT, ROM-автомат) — не двоить.
    // Legacy: старый stairPitchAdjust (реконструкция).
    if (!faElevZT())
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
    // ⭐ФОНАРЬ НЕ ТРОГАЕТ ПОЛ/ПОТОЛОК [VERIFIED 2026-07-28, сеттер 1d46/1d5e]: при активном фонаре ROM
    // делает ОБЫЧНЫЙ env-набор (`bsr 1d66` — тёмный fc-шаблон -$7152 остаётся!) и лишь ставит -$714c=16.
    // Конус — только на СТЕНАХ (подмена таблицы скейлеров cf36/d138). Прежний fcLit-конус (пол/потолок из
    // env0-шаблона в 16 колоннах) был НЕ-ROM аппроксимацией → с дальностью 16 давал яркую вертикальную
    // ПОЛОСУ через весь экран (юзер). УДАЛЁН.
    // КАРТА ВЫСОТ ПОЛА ЛЕСТНИЦЫ (кэш по этажу): пол клеток-спуска опущен → наклонный/смещённый вниз пол.
    static std::vector<int> stairHt; static int stairHtFloor = -1; static const Level* stairHtLvl = nullptr;
    if (stairHtFloor != cam.floor || stairHtLvl != &lvl) {
        buildStairHeightMap(lvl, cam.floor, stairHt); stairHtFloor = cam.floor; stairHtLvl = &lvl;
    }

    std::vector<uint32_t> nat((size_t)NW * NH);
    std::vector<uint32_t> natLo((size_t)NW * NH, 0);   // within-byte дизер: lo-шейд (правый пиксель байта)
    std::vector<double>  zc(NW, 0.0);
    faWallLo() = natLo.data();                          // faWallStrip пишет сюда правый px; блит читает для нечётного x

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
    const uint8_t camCt = (camCX >= 0 && camCY >= 0 && camCX < lvl.W && camCY < lvl.H)
                          ? lvl.cellType(cam.floor, camCX, camCY) : 0;
    const bool camOnStair = isStairFloorCT(camCt) || isStairProfileCT(camCt);
    // КАМЕРА ВНУТРИ ШАХТЫ ЛИФТА (ROM-рендер лифта). Синий пол/потолок/межэтажье + неподвижная шахта
    // рисуются ТОЛЬКО когда игрок в лифте (иначе синий «торчал через всю карту» — фидбэк). void внутри = синий.
    const bool camInElevZT = faElevZT() && rcElevCell(camCt);
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
    for (int cy = 0; cy < lvl.H && !faNoWalls() && !faRayDDA(); ++cy) {   // faNoWalls: пропуск стен; faRayDDA: DDA-путь ниже
        if (cy < camCY - drawDist || cy > camCY + drawDist) continue;
        for (int cx = 0; cx < lvl.W; ++cx) {
            if (cx < camCX - drawDist || cx > camCX + drawDist) continue;
            uint8_t ct = lvl.cellType(cam.floor, cx, cy);
            if (!cellRenderWall(ct)) continue;
            uint8_t cell = lvl.cellId(cam.floor, cx, cy);
            auto openN = [&](int nx, int ny) {
                if (nx < 0 || ny < 0 || nx >= lvl.W || ny >= lvl.H) return false;
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

            // Лифт (ROM §9.3): стена ШАХТЫ 0x30 = неподвижна (профиль +cabin гасит питч → noPitch) + над/под
            // срубом СПЛОШНОЙ СИНИЙ idx1 (0xd074 = пол/потолок лифта), рисуется по CELLTYPE (не по elevState).
            // Legacy (faElevZT off): прежний cabinCol по elevState, без синего над/под.
            const bool romShaft = camInElevZT && (ct == 0x30);   // синий над/под + неподвижность — только в шахте
            const bool isCabin  = faElevZT() ? romShaft : ((cam.elevState != 0) && rcElevCell(ct));
            // ПРОФИЛЬ-СКОС ЛЕСТНИЦЫ (ROM §9.1): по celltype+знаку cabin — 4 слова граней (L,R сдвиги вершин).
            // Передаём (L,R) грани через descD0/descD1; faDrawSeg сдвигает ГЕОМЕТРИЮ колонны + синий над/под.
            // ПРОФИЛЬ/СИНИЙ только когда камера НА лестнице (camOnStair) — иначе скос/синий «торчат через карту»
            // и стены пролёта, видимые издалека, скошены (как у лифта camInShaft). Снаружи — обычные стены.
            const bool stairCell = faElevZT() && camOnStair && isStairProfileCT(ct) && !faStairOff();
            // ЗНАК кабины: >0 подъём / <0 спуск / ==0 «на этаже, ДО спуска» (==0-ветка ROM: отображение до спуска).
            const int cabinSign = (cam.cabin > 0.0) ? 1 : (cam.cabin < 0.0 ? -1 : 0);
            const int sdx = cx - camCX, sdy = cy - camCY;   // смещение клетки от камеры (d0/d1 хендлера)
            (void)camOnStair;
            // L,R профиля грани face (0=N,1=W,2=E,3=S); {0,0} если не профиль-клетка.
            auto pf = [&](int face, int& L, int& R) { L = R = 0; if (stairCell) stairProfile(ct, face, cabinSign, sdx, sdy, L, R); };
            int pL, pR;
            // North ребро (сосед сверху открыт) -> грань 0 (flipU=true, иначе зеркалит)
            if (openN(cx, cy - 1)) { pf(0, pL, pR);
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy, cx + 1, cy, metaOf(0), true, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, romShaft, (double)pL, (double)pR); }
            // South ребро -> грань 3 (flipU=false)
            if (openN(cx, cy + 1)) { pf(3, pL, pR);
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy + 1, cx + 1, cy + 1, metaOf(3), false, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, romShaft, (double)pL, (double)pR); }
            // West ребро (видно с востока, +X) -> грань 1 (flipU=false, иначе зеркалит)
            if (openN(cx - 1, cy)) { pf(1, pL, pR);
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx, cy, cx, cy + 1, metaOf(1), false, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, romShaft, (double)pL, (double)pR); }
            // East ребро (видно с запада, -X) -> грань 2 (flipU=true)
            if (openN(cx + 1, cy)) { pf(2, pL, pR);
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          cx + 1, cy, cx + 1, cy + 1, metaOf(2), true, doShade, ramps, 0.0, 128.0, isCabin,
                          -1, false, false, false, 0, 0x40, romShaft, (double)pL, (double)pR); }
        }
    }

    // ── ДОСТОВЕРНЫЙ ПЕР-КОЛОНОЧНЫЙ SUPERCOVER-DDA (faRayDDA). Веер лучей как оригинал bc90: на КОЛОНКУ — луч,
    // шагает по клеткам (Amanatides-Woo) до 1-й непрозрачной грани. Реализовано (сверено с дизасмом render3d):
    //   • ШАГ1 осевые стены + текстура (texU как бокс-обход: N/E зеркалятся flipU) + тень + питч-сдвиг;
    //   • ШАГ2 supercover-углы (на угловом задевании пробуем ОБЕ боковые клетки — «боковые полосы за столбом»);
    //   • ШАГ3 диагонали 0x02-0x05 = ТОНКАЯ ОДНОСТОРОННЯЯ гипотенуза (тест пересечения линии, грань/намотка
    //     по хендлерам c8a8/c910/c97c/c842; вид со СПЛОШНОЙ стороны = плоская грань ребра);
    //   • ШАГ4 сдвижные двери (перекрытие створок по doorOpen) + профиль-скос лестницы (stairProfile→pshift).
    // Транзит-сегменты пол/потолок (shaftSeg/deferFC) рисуют отдельные проходы ниже (гейт reachedTransit) —
    // общие для бокс-обхода и DDA. ⭐ДЕФОЛТ ON (tuning.hpp faRayDDA; бокс-обход = легаси-фолбэк faRayDDA OFF).
    if (faRayDDA() && !faNoWalls()) {
        const double yc2 = NH / 2.0;
        const int maxSteps = drawDist * 2 + 2;
        const int cabinSign = (cam.cabin > 0.0) ? 1 : (cam.cabin < 0.0 ? -1 : 0);
        const bool elevRideG = (cam.elevState != 0);
        // ПИТЧ-СДВИГ обычной (не профиль/не шахта) колонны: как бокс-обход/faDrawSeg (D0·pitch/64). На ровном
        // полу pitch=0 → 0 (≡ ШАГ1). При прыжке/приседе/лифте/лестнице вид сдвигается вертикально (парити).
        auto normPitchShift = [&](int D0) -> int {
            double s = faTransitZT() ? 1.0 : (elevRideG ? 1.0 : faStairUni());
            return (int)(D0 * cam.pitch * s / 64.0);
        };
        // РИСУЕТ ОДНУ КОЛОНКУ СТЕНЫ: тексель-полоса высоты D0=64/perp, топ со сдвигом pshift, опц. синий бэкдроп
        // над/под (профиль лестницы / шахта лифта §9.2). Тень/дизер/полоса — как faDrawSeg (bandForHeight, CLUT).
        auto emitCol = [&](int ix, double perp, const uint8_t* mt, int tmx, int pshift, bool blueBk, bool elevRide) {
            if (perp < FA_NEAR) perp = FA_NEAR;
            double iv = 1.0 / perp;
            // fxProj (этап 1): D0 целочисленно как МД ca4c. fxZQuant (этап 2): глубина колонны (z-тест и
            // запись в zc) на СЕТКЕ целого D0 (=D0/64) — ROM держит per-column СКЕЙЛ стены ($FF6126..6226).
            int D0 = fxProj() ? fxD0FromPerp(perp) : (int)(faFocalV() * iv);
            if (fxZQuant() && D0 >= 1) iv = D0 / faFocalV();
            if (iv <= zc[ix]) return;
            if (D0 < 1) { zc[ix] = iv; return; }
            bool shadeCol = doShade;
        int band = 0;
        if (doShade) {
            if (faJuneBands()) { int jb = juneBandForHeight(D0); if (jb < 0) shadeCol = false; else band = jb; }
            else band = faWallBand0() ? 0 : bandForHeight(D0);
        }
            // ⭐ФОНАРЬ [VERIFIED 2026-07-28, cf36/d138]: конус = границы -$6f96..-$6f92 (13182:
            // $FF6196+прицел·4, ширина 0x20 = 16 z-колонн из 128) подменяют ТАБЛИЦУ СКЕЙЛЕРОВ -$7156
            // на 0x4436 (env0 Bright = блоки БЕЗ CLUT) → в конусе затенение ВЫКЛ (сырая палитра),
            // не «band 0» (band 0 при faWallBand0 и так дефолт → прежний код был no-op, конус пропал).
            if (flActive() && ix >= NW / 2 - NW / 16 && ix < NW / 2 + NW / 16) shadeCol = false;
            bool clutD = faWallDither() && (D0 <= faWallDitherD0());
            const int Dh = D0 * faWallHMul();                        // ⭐June: высота колонны 2×D0 [c812]
            int top = (int)yc2 - Dh / 2 + pshift;
            if (blueBk) {                                             // синий пол/потолок-задник (§9.2): над/под колонной
                // ⭐ROM d28e (сдвинутая/высокая колонна): синий НЕ с обеих сторон всегда.
                //   d278: сруб ПОЛНОСТЬЮ за экраном (D0/2±pshift ≤ −0x28) → ВСЯ колонка = полоса (синий), текстуры нет;
                //   d35c: pshift ≥ +0x28 → синий ТОЛЬКО сверху (текстура тянется до низа вида, низ НЕ льётся);
                //   d3b6: pshift ≤ −0x28 → синий ТОЛЬКО снизу;
                //   иначе (умеренный сдвиг) → оба зазора из полосы, как было.
                // До фикса порт лил синий с обеих сторон всегда → «синяя полоса вверх и вниз» на профиль-стенах
                // (центральная ячейка лестницы) и просвет уровня в поездке лифта. Тумблер faBlueD28E (дефолт ON).
                const int HALF = NH / 2;                              // 0x28 в ROM-единицах (вид 0x50 строк)
                bool fullOff = faBlueD28E() && (Dh / 2 - pshift <= -HALF || Dh / 2 + pshift <= -HALF);
                if (fullOff) {
                    for (int y = 0; y < NH; ++y) nat[(size_t)y * NW + ix] = FA_ELEV_BLUE;
                    zc[ix] = iv; return;                              // сруб не рисуем (он за экраном)
                }
                bool topBlue = !faBlueD28E() || (pshift > -HALF);     // d3b6: сильный сдвиг ВВЕРХ → верх не льём
                bool botBlue = !faBlueD28E() || (pshift <  HALF);     // d35c: сильный сдвиг ВНИЗ → низ не льём
                int botStart = top + Dh;
                if (topBlue) for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[(size_t)y * NW + ix] = FA_ELEV_BLUE;
                if (botBlue) for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[(size_t)y * NW + ix] = FA_ELEV_BLUE;
            }
            // СВЕРХВЫСОКАЯ КОЛОННА (ROM cfb2/ce74, D0>0x50): NH строк из центра с Bresenham-шагом текселя (как
            // бокс-обход). Только без питча/бэкдропа (иначе центр смещён → падаем в общий цикл).
            if (Dh > 0x50 && pshift == 0 && !blueBk) {
                int cy0 = (int)yc2, upTex = 31, loTex = 32, d3 = Dh;
                for (int n = 0; n < NH / 2; ++n) {
                    int yu = cy0 - (n + 1), yd = cy0 + n;
                    if (yu >= 0 && yu < NH) faWallStrip(nat.data(), yu * NW + ix, mt, upTex * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutD);
                    if (yd >= 0 && yd < NH) faWallStrip(nat.data(), yd * NW + ix, mt, loTex * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutD);
                    d3 -= 0x40; if (d3 < 0) { if (upTex > 0) --upTex; if (loTex < 63) ++loTex; d3 += Dh; }
                }
                zc[ix] = iv; return;
            }
            for (int i = 0; i < Dh; ++i) {
                int y = top + i; if (y < 0 || y >= NH) continue;
                int ty = (int)((i + 0.5) * 64.0 / Dh); if (ty > 63) ty = 63;
                faWallStrip(nat.data(), y * NW + ix, mt, ty * 128, tmx, wallPal, ramps, band, shadeCol, elevRide, clutD);
            }
            zc[ix] = iv;
        };
        // ОСЕВАЯ ГРАНЬ клетки (mX,mY), вошли через сторону side (0=верт.линия→W/E, 1=гор.→N/S) на параметре tCross
        // (=перпенд. дистанция, forward-компонента луча=1). texU как бокс-обход: W/S прямые, N/E зеркальные (flipU).
        auto emitAxial = [&](int ix, int mX, int mY, int side, double tCross, double rdx, double rdy) {
            uint8_t ct = lvl.cellType(cam.floor, mX, mY);
            double hitw = (side == 0) ? (cam.py + tCross * rdy) : (cam.px + tCross * rdx);
            double frac = hitw - std::floor(hitw);
            int stX = rdx > 0 ? 1 : -1, stY = rdy > 0 ? 1 : -1;
            int face; double tf;
            if (side == 0) { if (stX > 0) { face = 1; tf = frac; }     else { face = 2; tf = 1.0 - frac; } }   // W / E
            else           { if (stY > 0) { face = 0; tf = 1.0 - frac; } else { face = 3; tf = frac; } }        // N / S
            int tmx = fxProj() ? fxTexel(tf) : ((int)(tf * 128.0) & 127);   // fx: тексель со снапом к суб-поз 8.8
            uint8_t cell = lvl.cellId(cam.floor, mX, mY);
            uint16_t m = lvl.texorder(cell, face); if (m == 0 && cellIsDoor(ct)) m = DOOR_METATEX;
            const uint8_t* mt = meta.get(m);
            double perp = tCross < FA_NEAR ? FA_NEAR : tCross;
            int D0 = fxProj() ? fxD0FromPerp(perp) : (int)(faFocalV() / perp);
            int pshift; bool blueBk = false, elevRide = false;
            // ГЕЙТ по клетке камеры (camInElevZT/camOnStair) даёт «блочность» (dead_ends Тупик 11). При
            // faTransitNoCamGate профиль/шахта рисуются по ГЕОМЕТРИИ КЛЕТКИ (по дистанции, z-тест) — как ROM.
            const bool romShaft  = (faTransitNoCamGate() ? faElevZT() : camInElevZT) && (ct == 0x30);
            const bool stairCell = faElevZT() && (faTransitNoCamGate() || camOnStair) && isStairProfileCT(ct) && !faStairOff();
            if (romShaft) {
                // ⭐ROM 9862: профиль ВСЕХ 4 граней шахты = cabin|1 → pshift=(D0·pitch + D0·(cabin|1))/64.
                // Поездка (pitch=−cabin) → шахта неподвижна (остаток D0/64 = ROM, бит0-маркер); прыжок/присед
                // (cabin=0) → шахта едет ВМЕСТЕ с миром (жёсткий pshift=0 морозил её → относительный сдвиг).
                if (faShaftProf()) {
                    int cb = ((int)std::lround(cam.cabin)) | 1;
                    pshift = (int)((D0 * cam.pitch + (double)cb * D0) / 64.0);
                } else pshift = 0;                                               // legacy: жёстко неподвижна
                blueBk = true; elevRide = true;
            }
            else if (stairCell) {                                                // профиль-скос лестницы (ROM d214)
                int L = 0, R = 0;
                stairProfile(ct, face, cabinSign, mX - camCX, mY - camCY, L, R);
                // ⭐ROM ce0a: `tst -0x6e8e; bne d0c4` — грань с НУЛЕВЫМ словом профиля = ОБЫЧНАЯ стена
                // (градиент-заливка d186/d202, БЕЗ синего). Синий задник — только у граней с ненулевым
                // словом (путь d0c4, a5=$d074). До фикса порт лил синий на ВСЕ грани профиль-клеток →
                // лишний синий на гранях с word=0 (напр. S-грань 0x11 при cabin=0). Гейт под faBlueD28E.
                if (faBlueD28E() && L == 0 && R == 0) { pshift = normPitchShift(D0); }
                else {
                    const bool flipUface = (face == 0 || face == 2);             // N/E: L/R на конце texU=0 меняются (faStairFaceLR)
                    double pb = flipUface ? (R + (L - R) * frac) : (L + (R - L) * frac);   // профиль-байт по доле грани
                    double sign = faStairFlip() ? -1.0 : 1.0;
                    pshift = (int)((D0 * cam.pitch * sign + pb * D0) / 64.0);
                    blueBk = true;
                }
                if (std::getenv("ZTDBG_COL"))
                    std::fprintf(stderr, "col ix=%d cell=(%d,%d) ct=%02X face=%d L=%d R=%d frac=%.2f D0=%d pshift=%d blue=%d\n",
                                 ix, mX, mY, ct, face, L, R, frac, D0, pshift, blueBk ? 1 : 0);
            } else pshift = normPitchShift(D0);
            emitCol(ix, perp, mt, tmx, pshift, blueBk, elevRide);
        };
        // ДИАГОНАЛЬ 0x02-0x05: тонкая гипотенуза. Возвращает true=попал (стоп луча), false=прошёл (пустая сторона).
        // Со СПЛОШНОЙ стороны (вход в solid-треугольник) — плоская грань ребра (emitAxial по стороне входа).
        auto emitDiag = [&](int ix, int mX, int mY, int side, double tCross, double rdx, double rdy) -> bool {
            uint8_t ct = lvl.cellType(cam.floor, mX, mY);
            // точка входа в клетку (на tCross) — определяет, с какой стороны гипотенузы пришёл луч
            double exX = cam.px + tCross * rdx - mX, exY = cam.py + tCross * rdy - mY;
            bool solid = (ct == 2) ? (exX >= exY) : (ct == 4) ? (exX <= exY)
                       : (ct == 3) ? (exX + exY >= 1.0) : (exX + exY <= 1.0);
            if (solid) { if (tCross <= 1e-6) return false;                 // клетка камеры на сплошной стороне → пропуск (марш выйдет)
                         emitAxial(ix, mX, mY, side, tCross, rdx, rdy); return true; }   // вид со сплошной стороны = ребро
            // пересечение луча с линией гипотенузы внутри клетки
            const bool bslash = (ct == 2 || ct == 4);                     // «\» NW-SE : x−y=const ; «/» NE-SW : x+y=const
            double den = bslash ? (rdx - rdy) : (rdx + rdy);
            if (den < 1e-9 && den > -1e-9) return false;                  // луч параллелен диагонали
            double t = bslash ? ((mX - mY) - (cam.px - cam.py)) / den
                              : ((mX + mY + 1) - (cam.px + cam.py)) / den;
            if (t <= 0.0 || t < tCross - 1e-6) return false;
            double sx = cam.px + t * rdx - mX, sy = cam.py + t * rdy - mY;
            if (sx < -1e-6 || sx > 1.0 + 1e-6 || sy < -1e-6 || sy > 1.0 + 1e-6) return false;
            // намотка texU (P1→texU0) и грань текстуры по хендлерам: 0x02 NW→SE/юж.тек; 0x04 SE→NW/сев.тек;
            // 0x03 NE→SW/сев.тек; 0x05 SW→NE/юж.тек (сверено c8a8/c910/c97c/c842, dead_ends шаг3).
            double tf; int dface;
            switch (ct) { case 2: tf = sx;       dface = 3; break;
                          case 4: tf = 1.0 - sx; dface = 0; break;
                          case 3: tf = sy;       dface = 0; break;
                          default:tf = sx;       dface = 3; break; }   // 0x05
            int tmx = fxProj() ? fxTexel(tf) : ((int)(tf * 128.0) & 127);
            uint8_t cell = lvl.cellId(cam.floor, mX, mY);
            const uint8_t* mt = meta.get(lvl.texorder(cell, dface));
            double perp = t < FA_NEAR ? FA_NEAR : t;
            emitCol(ix, perp, mt, tmx, normPitchShift(fxProj() ? fxD0FromPerp(perp) : (int)(faFocalV() / perp)), false, false);
            return true;
        };
        // СДВИЖНАЯ ДВЕРЬ 6(гориз.)/7(верт.): панель в центре клетки, две створки уезжают к краям (текстура едет
        // с ними, как бокс-обход b036). Возвращает true=луч попал в створку, false=прошёл сквозь проём/полностью открыта.
        auto emitDoor = [&](int ix, int mX, int mY, double rdx, double rdy) -> bool {
            uint8_t ct = lvl.cellType(cam.floor, mX, mY);
            double h = doorOpen(cam.floor, mX, mY) * 0.5;                 // 0=закрыта .. 0.5=открыта
            if (h >= 0.499) return false;                                 // полностью открыта — проём
            double t, cf;                                                 // t=перпенд.дист, cf=доля вдоль панели (0..1)
            if (doorIsHoriz(ct)) { if (rdy < 1e-9 && rdy > -1e-9) return false;    // гориз. панель на y=mY+0.5 (обычн.0x06 / фейк 0x83)
                           t = (mY + 0.5 - cam.py) / rdy; if (t <= 0) return false; cf = cam.px + t * rdx - mX; }
            else         { if (rdx < 1e-9 && rdx > -1e-9) return false;    // верт. панель на x=mX+0.5 (0x07 / фейк 0x84)
                           t = (mX + 0.5 - cam.px) / rdx; if (t <= 0) return false; cf = cam.py + t * rdy - mY; }
            if (cf < 0.0 || cf > 1.0) return false;
            double texU;                                                  // створки показывают уехавшую часть текстуры
            if      (cf < 0.5 - h) texU = (h * 128.0) + (64.0 - h * 128.0) * (cf / (0.5 - h));            // левая/верхняя
            else if (cf > 0.5 + h) texU = 64.0 + ((128.0 - h * 128.0) - 64.0) * ((cf - (0.5 + h)) / (0.5 - h)); // правая/нижняя
            else return false;                                            // проём между створками — прошёл
            int tmx = fxProj() ? fxTexel(texU / 128.0) : ((int)texU & 127);   // fx: суб-поз 8.8 вдоль панели
            uint8_t cell = lvl.cellId(cam.floor, mX, mY);
            uint16_t m = lvl.texorder(cell, 0); if (m == 0) m = DOOR_METATEX;
            const uint8_t* mt = meta.get(m);
            double perp = t < FA_NEAR ? FA_NEAR : t;
            emitCol(ix, perp, mt, tmx, normPitchShift(fxProj() ? fxD0FromPerp(perp) : (int)(faFocalV() / perp)), false, false);
            return true;
        };
        // ПЕР-КОЛОНОЧНЫЙ ТРАНЗИТ-СЕГМЕНТ (ROM d5ce: сегмент рисуется В ТОМ ЖЕ марше на РЕАЛЬНОЙ дистанции луча до
        // клетки, НЕ по бинарному reachedTransit). Собираем ВСЕ достигнутые транзит-клетки колонны (как ROM-очередь
        // 9d72 — тайлятся в приёмистый пол/потолок; только ближняя давала дыры у горизонта). Рисуем ПОСЛЕ стены.
        const int SEG_MAX = 24;
        double segTd[SEG_MAX]; int segTy[SEG_MAX]; int segN = 0;
        // КЛАССИФИКАЦИЯ вошедшей клетки: диагональ / дверь / стена / транзит-сегмент / прозрачная.
        // true = луч остановлен (грань нарисована), false = продолжаем марш.
        auto processCell = [&](int ix, int mX, int mY, int side, double tCross, double rdx, double rdy) -> bool {
            if (mX < 0 || mY < 0 || mX >= lvl.W || mY >= lvl.H) return true;
            uint8_t ct = lvl.cellType(cam.floor, mX, mY);
            if (ct >= 2 && ct <= 5)  return emitDiag(ix, mX, mY, side, tCross, rdx, rdy);
            if (cellRendersDoor(ct)) return emitDoor(ix, mX, mY, rdx, rdy);   // обычн.двери + ФЕЙК-двери 0x83/0x84 (дверной рендер створок)
            if (cellBlocks(ct))     { emitAxial(ix, mX, mY, side, tCross, rdx, rdy); return true; }
            if (faElevZT() && segN < SEG_MAX) {                          // прозрачная транзит-клетка → в очередь сегментов
                // ⭐КАНОНИЧЕСКОЕ РЕБРО + НАПРАВЛЕННЫЙ ГЕЙТ (хендлеры 988a-9a4a, faSegGate): celltype пакует ровно
                // ОДНО фикс. ребро и только с одной стороны (строгий tst d0/d1). d5ce красит колонки [X1..X2] этого
                // ребра ⟺ луч ПЕРЕСЕКАЕТ его = вход в клетку именно через это ребро (side+знак шага; вход через
                // боковое ребро → колонка вне спана → ROM её не красит). Подход с тыла → сегмента НЕТ вовсе
                // (чинит «полосы с противоположной стороны» и «синий виден до прохода через дверь»).
                int edge = -1, st = segEdgeType(ct, edge);
                if (st && (rcElevCell(ct) || (faStairSteps() && !faStairOff()))) {
                    int entry = (side == 0) ? (rdx > 0 ? 2 : 3) : (rdy > 0 ? 0 : 1);   // ребро входа: W/E/N/S
                    if (!faSegGate() || entry == edge) { segTd[segN] = tCross; segTy[segN] = st; ++segN; }
                }
            }
            return false;                                                 // прозрачная → идём дальше
        };
        // ⭐fxFan (этап 3): веер ROM bb00 — rayAng = ($9124[col] + угол камеры) & 0x1ff, направление из zAngLUT.
        // Для per-column рендера направление нормируется к forward-компоненте 1 (деление на fdot=cos(оффсета)
        // — только bookkeeping, чтобы tCross=perp как прежде); КВАНТОВАНИЕ направления = ROM-сетка 512+LUT.
        // NW=128 колонн, таблица 0..0x80=129 лучей-ГРАНИЦ → колонка ix берёт левую границу fan[ix] (ROM красит
        // спан [screenX..): луч границы i попадает в screenX=i по построению веера — обратная пара к ca4c).
        const bool fanOn = fxFan() && zAngLUTok() && zFanLUTok() && NW == 128;
        int fanCamAng = 0; double fanCcs = 0, fanCsn = 0;
        if (fanOn) { fanCamAng = ((int)std::lround(camDirToAng512(cam)) + 128) & 511;
                     fanCcs = zAngLUT()[fanCamAng * 2]; fanCsn = zAngLUT()[fanCamAng * 2 + 1]; }
        for (int ix = 0; ix < NW; ++ix) {
            segN = 0;                                                   // сброс очереди транзит-сегментов на колонку
            double rdx, rdy;
            if (fanOn) {
                // центр колонки = среднее соседних лучей-границ (fan[ix]..fan[ix+1]), окр. вверх: граница
                // fan[ix] давала систематический полуколоночный сдвиг против сверенного float-центра.
                int ra = (fanCamAng + ((zFanLUT()[ix] + zFanLUT()[ix + 1] + 1) >> 1)) & 511;
                double cs = zAngLUT()[ra * 2], sn = zAngLUT()[ra * 2 + 1];
                double fdot = (cs * fanCcs + sn * fanCsn) / 65536.0;     // cos углового оффсета (≥ ~0.7 при ±45°)
                if (fdot < 0.05) fdot = 0.05;
                rdx = cs / 256.0 / fdot; rdy = sn / 256.0 / fdot;        // forward-компонента = 1 → tCross=perp
            } else {
                double pcol = (ix + 0.5 - NW * 0.5) / (NW * 0.5);        // lateral/forward колонки (FOV порта)
                rdx = cam.dirX - cam.dirY * pcol; rdy = cam.dirY + cam.dirX * pcol;   // forward-компонента = 1 → tCross=perp
            }
            int mapX = camCX, mapY = camCY;
            double adx = rdx < 0 ? -rdx : rdx, ady = rdy < 0 ? -rdy : rdy;
            double dDX = adx > 1e-9 ? 1.0 / adx : 1e30, dDY = ady > 1e-9 ? 1.0 / ady : 1e30;
            int stX = rdx > 0 ? 1 : -1, stY = rdy > 0 ? 1 : -1;
            double sX = (rdx > 0 ? (mapX + 1 - cam.px) : (cam.px - mapX)) * dDX;
            double sY = (rdy > 0 ? (mapY + 1 - cam.py) : (cam.py - mapY)) * dDY;
            const double CE = 1e-6;                                       // угловой эпсилон (луч через угол клетки)
            auto plainWall = [&](int cx, int cy) {                        // сплошная ОСЕВАЯ стена (не диаг/не дверь) для supercover-пробы
                if (cx < 0 || cy < 0 || cx >= lvl.W || cy >= lvl.H) return false;
                uint8_t c = lvl.cellType(cam.floor, cx, cy);
                return cellBlocks(c) && !(c >= 2 && c <= 5) && !cellIsDoor(c);
            };
            // КЛЕТКА КАМЕРЫ = диагональ (игрок вошёл в пустой треугольник / прижат к гипотенузе): марш пропускает
            // СВОЮ клетку → её гипотенуза не рисовалась → «видно нутро». Спец-обработка первой клетки (ROM bc90):
            // рисуем гипотенузу клетки камеры, если луч идёт в сплошную сторону (пересечение впереди, t>0).
            {
                uint8_t cc = (camCX >= 0 && camCY >= 0 && camCX < lvl.W && camCY < lvl.H)
                             ? lvl.cellType(cam.floor, camCX, camCY) : 0;
                if (cc >= 2 && cc <= 5 && emitDiag(ix, camCX, camCY, 1, 0.0, rdx, rdy)) continue;
            }
            for (int s = 0; s < maxSteps; ++s) {
                if (sX > sY - CE && sX < sY + CE) {                       // УГОЛ: supercover — пробуем ОБЕ боковые клетки
                    double tC = sX;
                    if      (plainWall(mapX + stX, mapY)) { emitAxial(ix, mapX + stX, mapY, 0, tC, rdx, rdy); break; }
                    else if (plainWall(mapX, mapY + stY)) { emitAxial(ix, mapX, mapY + stY, 1, tC, rdx, rdy); break; }
                    mapX += stX; mapY += stY; sX += dDX; sY += dDY;       // ни одна не стена → шаг по диагонали
                    if (processCell(ix, mapX, mapY, 1, tC, rdx, rdy)) break;
                } else if (sX < sY) {                                     // пересечение ВЕРТ. линии → шаг по X
                    double tC = sX; mapX += stX; sX += dDX;
                    if (processCell(ix, mapX, mapY, 0, tC, rdx, rdy)) break;
                } else {                                                  // ГОР. линия → шаг по Y
                    double tC = sY; mapY += stY; sY += dDY;
                    if (processCell(ix, mapX, mapY, 1, tC, rdx, rdy)) break;
                }
            }
            // ТРАНЗИТ-СЕГМЕНТЫ этой колонки (ROM d5ce, deferFC): на клетку 2 спана — потолок [0,top) + пол
            // [top+D0,NH), источник синий idx1 / ГРАДИЕНТ-маркер; середина [top,top+D0) ПРОЗРАЧНА (там дальняя
            // стена/фон). Рисуем ВСЕ достигнутые (тайлятся в приёмистый пол/потолок) от ДАЛЬНЕЙ к БЛИЖНЕЙ
            // (ближняя перекрывает), z-тест против стены (d5ce cmp/bcs: сегмент рисуется, только если БЛИЖЕ стены).
            for (int si = segN - 1; si >= 0; --si) {
                double perp = segTd[si] < FA_NEAR ? FA_NEAR : segTd[si];
                double iv = 1.0 / perp;
                int D0 = fxProj() ? fxD0FromPerp(perp) : (int)(faFocalV() / perp);
                if (fxZQuant() && D0 >= 1) iv = D0 / faFocalV();         // этап 2: сегмент на той же D0-сетке
                if (iv < zc[ix]) continue;                               // за стеной → пропуск
                if (D0 < 1) continue;
                int top = (int)(NH / 2.0) - D0 / 2 + normPitchShift(D0);
                uint32_t src = (segTy[si] == 1) ? FA_ELEV_BLUE : 0x00000001u;   // синий idx1 / FA_GRAD (пол-потолок в блите)
                int botStart = top + D0;
                for (int y = 0;        y < top && y < NH; ++y) if (y >= 0) nat[(size_t)y * NW + ix] = src;
                for (int y = botStart; y < NH;            ++y) if (y >= 0) nat[(size_t)y * NW + ix] = src;
            }
        }
    }

    // ── ДОСТИЖИМОСТЬ КЛЕТОК ЛУЧОМ (ROM bc90: транзит-клетка пакуется ⟺ DDA-луч ВОШЁЛ в неё ДО непрозрачной
    // стены). Пер-КОЛОНОЧНЫЙ DDA (как оригинал: 0x80 лучей), клетка помечается при ВХОДЕ луча (не центр —
    // иначе край не «дотягивается»). Проходит сквозь прозрачные клетки (транзит-пол, пусто), стоп на стене.
    // reachedTransit[cy*W+cx]=1 → клетка достигнута. Заменяет пер-клеточный LOS (клиппился об углы стен).
    static std::vector<uint8_t> reachedTransit;
    reachedTransit.assign((size_t)lvl.W * lvl.H, 0);
    {
        const int ccx = (int)cam.px, ccy = (int)cam.py;
        if (ccx >= 0 && ccy >= 0 && ccx < lvl.W && ccy < lvl.H)
            reachedTransit[(size_t)ccy * lvl.W + ccx] = 1;         // клетка камеры
        const int NR = NW * 2;                                        // 2× плотность (меньше промахов по углам/периферии)
        const int maxSteps = drawDist * 2 + 2;
        for (int ir = 0; ir <= NR; ++ir) {
            // FOV ШИРЕ вида (×1.4): краевые/периферийные клетки (центр за 90°-FOV, но край виден) тоже метятся;
            // переразметка безопасна — сегменты клипятся фрустумом в faDrawSeg. Ловит «слева/справа не добивает».
            double p = (ir - NR * 0.5) / (NR * 0.5) * 1.4;            // lateral/forward ∈ [-1.4,1.4]
            double rdx = cam.dirX - cam.dirY * p, rdy = cam.dirY + cam.dirX * p;
            double adx = rdx < 0 ? -rdx : rdx, ady = rdy < 0 ? -rdy : rdy;
            int cx = ccx, cy = ccy, stepX = rdx > 0 ? 1 : -1, stepY = rdy > 0 ? 1 : -1;
            double tMaxX = adx > 1e-9 ? ((stepX > 0 ? (cx + 1 - cam.px) : (cam.px - cx)) / adx) : 1e30;
            double tMaxY = ady > 1e-9 ? ((stepY > 0 ? (cy + 1 - cam.py) : (cam.py - cy)) / ady) : 1e30;
            double tDeltaX = adx > 1e-9 ? 1.0 / adx : 1e30, tDeltaY = ady > 1e-9 ? 1.0 / ady : 1e30;
            // tryMark: метит клетку достижимой; возвращает true если клетка = СТОП (сплошная стена / закрытая дверь).
            // Дверь: ЗАКРЫТАЯ блокирует (лифт/лестницу за ней не видно — норма), ОТКРЫТАЯ (анимация при подходе)
            // пропускает → плавное появление через растущий проём (z-тест по щели).
            auto tryMark = [&](int mx, int my) -> bool {
                if (mx < 0 || my < 0 || mx >= lvl.W || my >= lvl.H) return true;
                uint8_t mct = lvl.cellType(cam.floor, mx, my);
                if (cellBlocks(mct)) return true;
                if (cellIsDoor(mct) && doorOpen(cam.floor, mx, my) < 1e-3) return true;
                reachedTransit[(size_t)my * lvl.W + mx] = 1;
                return false;
            };
            for (int s = 0; s < maxSteps; ++s) {
                if (tMaxX < tMaxY - 1e-9) {          // пересечение ВЕРТ. линии сетки → шаг по X
                    cx += stepX; tMaxX += tDeltaX;
                    if (tryMark(cx, cy)) break;
                } else if (tMaxY < tMaxX - 1e-9) {   // ГОР. линия → шаг по Y
                    cy += stepY; tMaxY += tDeltaY;
                    if (tryMark(cx, cy)) break;
                } else {                             // УГОЛ (луч идёт через угол клетки) — SUPERCOVER: задеть ОБЕ
                    tryMark(cx + stepX, cy);         //   боковые клетки (не стоп на задевании — как оригинал), затем
                    tryMark(cx, cy + stepY);         //   шаг по диагонали. Это чинит «боковые полосы за столбом».
                    cx += stepX; cy += stepY; tMaxX += tDeltaX; tMaxY += tDeltaY;
                    if (tryMark(cx, cy)) break;      // диагональная клетка: стоп если стена
                }
            }
        }
    }
    auto isReached = [&](int cx, int cy) { return reachedTransit[(size_t)cy * lvl.W + cx] != 0; };
    if (std::getenv("RDBG")) {   // дебаг: сетка достижимости вокруг камеры (R=достигнута, #=стена, .=нет)
        int ccx = (int)cam.px, ccy = (int)cam.py;
        std::fprintf(stderr, "REACH cam(%d,%d) dir(%.2f,%.2f):\n", ccx, ccy, cam.dirX, cam.dirY);
        for (int yy = ccy - 7; yy <= ccy + 7; ++yy) {
            std::fprintf(stderr, "y%3d ", yy);
            for (int xx = ccx - 7; xx <= ccx + 7; ++xx) {
                if (xx < 0 || yy < 0 || xx >= lvl.W || yy >= lvl.H) { std::fprintf(stderr, "  ."); continue; }
                uint8_t ct = lvl.cellType(cam.floor, xx, yy);
                char r = reachedTransit[(size_t)yy * lvl.W + xx] ? 'R' : (cellBlocks(ct) ? '#' : '.');
                std::fprintf(stderr, "%c%02X", r, ct);
            }
            std::fprintf(stderr, "\n");
        }
    }

    // ── ROM-СЕГМЕНТЫ ЛИФТА (§9.3, флаш 9d72 ПОСЛЕ DDA-обхода): кабина/стойки (0x32-34/51-5b) = сегмент-стена
    // с ГРАДИЕНТОМ сверху/снизу и ПРОЗРАЧНОЙ серединой (сквозь неё уезжает мир питчем); area (0x31/50/54/58)
    // = сегмент-ПОЛ сплошного синего idx1. deferFC: заливает 2 спана (потолок+пол), середину D0 не трогает.
    // Рисуем ребро клетки, ОБРАЩЁННОЕ К КАМЕРЕ (заднее отсекается frustum/near-клипом). z-тест против стен (overdraw).
    if (faElevZT() && !faNoWalls() && !faRayDDA()) {                  // (только бокс-обход; в DDA — пер-колоночно в марше)
        for (int cy = camCY - drawDist; cy <= camCY + drawDist; ++cy) {
            if (cy < 0 || cy >= lvl.H) continue;
            for (int cx = camCX - drawDist; cx <= camCX + drawDist; ++cx) {
                if (cx < 0 || cx >= lvl.W) continue;
                uint8_t ct = lvl.cellType(cam.floor, cx, cy);
                if (!rcElevCell(ct) || ct == 0x30) continue;          // 0x30 (шахта) уже нарисована в обходе стен
                if (!isReached(cx, cy)) continue;                     // достижимость лучом (не за стеной)
                // Каноническое ребро + направленный гейт хендлеров 997a-9a4a (как слой ступеней ниже) —
                // прежний выбор «ближних» рёбер по позиции камеры рисовал с любой стороны и по 2 ребра сразу.
                int ex0, ey0, ex1, ey1;
                int type = shaftSeg(ct, cx, cy, camCX, camCY, ex0, ey0, ex1, ey1);
                if (type == 0) continue;
                const int sIdx = (type == 1) ? 1 : -1;                // area → синий (solidIdx>=0); кабина → градиент (FA_GRAD)
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          ex0, ey0, ex1, ey1, 0, false, doShade, ramps, 0.0, 128.0,
                          false, sIdx, false, /*overdraw*/true, /*deferFC*/true, 0, 0x40, false, 0.0, 0.0);
            }
        }
    }

    // ── СЛОЙ B: СТУПЕНИ-СЕГМЕНТЫ ЛЕСТНИЦЫ (ROM 988a-9a4a → 9d42/9d4a → d5ce). Клетки-пол лестницы (0x12-0x17,
    // 0x3c-0x4f) рисуют ОДНО ребро (shaftSeg: риза-градиент type2 / синий пол type1) отложенным сегментом
    // deferFC (2 спана над/под срубом высоты D0, сдвиг D0·pitch/64, середина прозрачна).
    // ГЕЙТ = ДОСТИЖИМОСТЬ ЛУЧОМ (cellReachable) + per-column z-тест (overdraw против zc стен) — как ROM
    // (bc90: сегмент клетки в очереди ⟺ луч дошёл до неё до стены; d5ce: z-тест). НЕ «камера на клетке» —
    // это давало резкое появление. Достижимость → плавное появление по мере приближения, без торчания сквозь карту.
    if (faElevZT() && faStairSteps() && !faNoWalls() && !faStairOff() && !faRayDDA()) {   // (только бокс-обход; в DDA — в марше)
        for (int cy = camCY - drawDist; cy <= camCY + drawDist; ++cy) {
            if (cy < 0 || cy >= lvl.H) continue;
            for (int cx = camCX - drawDist; cx <= camCX + drawDist; ++cx) {
                if (cx < 0 || cx >= lvl.W) continue;
                uint8_t ct = lvl.cellType(cam.floor, cx, cy);
                int ex0, ey0, ex1, ey1;
                int type = shaftSeg(ct, cx, cy, camCX, camCY, ex0, ey0, ex1, ey1);  // 0=нет, 1=синий, 2=градиент
                if (type == 0) continue;
                if (!isReached(cx, cy)) continue;                     // достижимость лучом (не за стеной)
                const int sIdx = (type == 1) ? 1 : -1;                    // синий пол (solidIdx>=0) / градиент-риза (FA_GRAD)
                faDrawSeg(nat.data(), NW, NH, zc.data(), lvl, wallPal, meta, cam,
                          ex0, ey0, ex1, ey1, 0, false, doShade, ramps, 0.0, 128.0,
                          false, sIdx, false, /*overdraw*/true, /*deferFC*/true, 0, 0x40, false, 0.0, 0.0);
            }
        }
    }

    // ДЕКОР/ВРАГИ-БИЛБОРДЫ: при faDecorFullRes (дефолт) — ПОСЛЕ блита в ПОЛНОМ разрешении (см. ниже, как VDP-
    // спрайты оригинала). При выкл. — СТАРЫЙ путь в nat=128 (удваивается блитом = half-res, для сравнения).
    if (!faDecorFullRes() && faDecor() && !faWallsOnly() && meta.obj && meta.obj->count > 0)
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
    // ⭐СНАЙПЕР-ОВЕРЛЕЙ (ROM 2404): здание/снайпер/трассер = VDP-спрайты pri=0 → НАД панорамой,
    // ПОД 3D-видом — сэмплим тут же, только там, где виден фон (sniper_overlay.hpp).
    auto bgAt = [&](int x_, int sy) -> uint32_t {
        if (uint32_t sp = snip::sampleLow(x_, W, sy, H)) return sp;
        return hasBg ? bgSample(*bg, cam.dirX, cam.dirY, cam.floor, cam.cabin, x_, W, sy, fbHorizon) : 0xFF000000u;
    };
    // ПОЛ/ПОТОЛОК-градиент (Plane B): точный дизер-паттерн ZT в ПОЛНОМ 256-широком ZT-виде.
    // ⭐ Дизер градиента — ЭКРАННЫЙ, разрешение 256 (128 байт-колонок × 2px hi/lo), как d202: на каждую из
    // 128 байт-колонок пишется байт gradient[row] (2px), фаза чёт/нечёт. Нативный буфер стен = 128 геом-колонн
    // (удваивается блитом до 256, hi/lo из nat/natLo). Градиент НЕ проходит через nat/natLo → семплим прямо
    // по 256-коорду: gx = 2·(нат.позиция) → байт-колонка = нат.колонка, два удвоенных px = hi/lo нибблы.
    // (РЕГРЕССИЯ до фикса: семплили по nx=0..127 → байт-кол=nx>>1=0..63 → период дизера ВДВОЕ грубее.)
    auto gradAt = [&](int x_, int sy) -> uint32_t {
        double nxf = cxN + (x_ - cxF) / (scX * hs);
        int gx = (int)(2.0 * nxf);                     if (gx < 0) gx = 0; if (gx > 255) gx = 255;  // 256-широкий ZT-коорд
        int ny = sy * NH / H;                          if (ny < 0) ny = 0; if (ny > NH - 1) ny = NH - 1;
        int r  = ny * 80 / NH;                         if (r > 79) r = 79;
        // ROM-точный путь: градиент НЕ статичен — пересэмплирован по питчу (D4A6, $FF6226).
        if (faTransitZT()) r = fcRowD4A6(r, (int)cam.pitch);
        // (фонарь пол/потолок НЕ освещает — 1d5e: env-шаблон остаётся тёмным; конус только на стенах)
        return floorCeilColorExact(wallPal, fc, gx, r);
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
    const bool camInShaft = (camCX >= 0 && camCY >= 0 && camCX < lvl.W && camCY < lvl.H) &&
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
        if (wcx < 0 || wcy < 0 || wcx >= lvl.W || wcy >= lvl.H) return false;
        uint8_t ct = lvl.cellType(cam.floor, wcx, wcy);
        if (rcElevCell(ct)) return true;                            // лифт: пол/потолок-void синий
        // ЛЕСТНИЦА: синий ПОЛ спуска (только низ, dn>0) там, где под полом клетка-лестница (флор-каст legacy).
        // При faStairSteps ON пол/ризы рисуются СЕГМЕНТАМИ (слой B) — floor-cast лестницы ВЫКЛ (иначе двойной синий).
        if (camOnStair && !faStairSteps() && dn > 0 && (isStairFloorCT(ct) || isStairProfileCT(ct))) return true;
        return false;
    };
    // ВЫСОТА КАМЕРЫ (pitch) = сдвиг ВСЕГО вида целиком (стены+пол+потолок двигаются вместе → нет наклона).
    // ОБЩИЙ механизм с прыжком/приседом (по дизасму -71e6 — высота камеры, easing ±4/кадр, сброс 0 при загрузке).
    // Лифт НЕ трогаем (свой per-column): viewShiftPx=0 при elevState. Источник берём со сдвигом, назначение — y.
    // ROM-точный путь: whole-view shift ВЫКЛ (в ROM его нет — только per-column d214 + D4A6-градиент).
    // ОТЛАДКА: дамп нативного индекс-буфера стен (почисленная сверка с оригиналом ZT). nat=hi, natLo=lo.
    if (faDumpIdxPath()) {
        FILE* df = std::fopen(faDumpIdxPath(), "w");
        if (df) {
            auto rev = [&](uint32_t c) -> int {                     // ARGB -> палитр-индекс 0..15 (или -1)
                if ((c & 0xFF000000u) == 0) return -1;              // маркер/прозрачно (FA_GRAD/FA_BG/окно)
                for (int i = 0; i < 16; ++i) if (wallPal.c[i] == c) return i;
                return -2;                                          // цвет не из wallPal (пол/фон/лифт)
            };
            std::fprintf(df, "%d %d\n", NW, NH);                    // размер нативного буфера
            for (int y = 0; y < NH; ++y) {
                for (int x = 0; x < NW; ++x) {
                    int hi = rev(nat[(size_t)y * NW + x]);
                    int lo = rev(natLo[(size_t)y * NW + x]);
                    std::fprintf(df, "%d,%d ", hi, lo);
                }
                std::fprintf(df, "\n");
            }
            std::fclose(df);
        }
    }
    const int viewShiftPx = (faTransitZT() || cam.elevState != 0) ? 0 : (int)(cam.pitch * faStairUni());
    for (int x = 0; x < W; ++x) {
        int nx = (int)(cxN + (x - cxF) / (scX * hs));
        if (nx < 0) nx = 0; if (nx > NW - 1) nx = NW - 1;
        double iv = zc[nx];
        zbuf[x] = (iv > 0) ? 1.0 / iv : 1e9;
        // Лифт: колонна = «стена кабины» (rcElevCell → синий пол/потолок) или «вид на выход» (коридор).
        // ⚠ РЕКОНСТРУКЦИЯ (не ROM) — только legacy (faElevZT off). ROM-путь: синий пол/потолок из shaftWall
        // (стена шахты 0x30) прямо в nat, void = градиент-фон. camInShaft/mode/band не применяются.
        int mode = 0;                                 // 0 обычная, 1 коридор-лифт, 2 кабина-лифт
        if (transit && !faElevZT()) mode = rcElevCell(rcColumnHitCt(lvl, cam, x, W, hs)) ? 2 : 1;
        for (int y = 0; y < H; ++y) {
            // Межэтажный синий band-слой — РЕКОНСТРУКЦИЯ (не ROM); в ROM-точном пути его нет:
            // «синий» при поездке даёт D4A6-ресемпл градиента (середина полос = idx2) + синие сегменты.
            if (!faTransitZT() && transit && mode == 1 && y >= blueLo && y < blueHi) { put(x, y, VOID_BLUE); continue; } // межэтажный синий
            int sy = y - viewShiftPx;                  // сдвиг ВСЕГО вида по высоте камеры (источник)
            if (sy < 0) sy = 0; else if (sy >= H) sy = H - 1;       // край вытягивается (без чёрных полос)
            uint32_t v = nat[(size_t)(sy * NH / H) * NW + nx];
            uint32_t c;
            if ((v & 0xFF000000u) != 0) {                          // непрозрачная стена (Plane B)
                c = v;                                             // ЛЕВЫЙ пиксель байта = hi-шейд
                if (x & 1) {                                       // ПРАВЫЙ пиксель байта = lo-шейд (within-byte дизер VDP)
                    uint32_t vlo = natLo[(size_t)(sy * NH / H) * NW + nx];
                    if ((vlo & 0xFF000000u) != 0) c = vlo;         // (0 = не стена-тексель, оставляем hi)
                }
            }
            else if (faWallsOnly())              c = wallPal.c[faDbgBgIdx() & 0xF]; // отладка: пол/потолок/фон = сплошной
            else if (transit && mode == 2)       c = VOID_BLUE;    // кабина лифта → синий
            else if (v == FA_GRAD) {                               // не-стена → пол/потолок-градиент
                if (!faElevZT() && shaftFloorBlue(x, sy)) c = VOID_BLUE;  // legacy
                else {
                    c = gradAt(x, sy);
                    if ((c & 0xFF000000u) == 0) c = bgAt(x, sy);   // idx0 (env3-небо) → ФОН (Plane A)
                }
            } else                               c = bgAt(x, sy);  // FA_BG: окно (тексель-0) → ФОН
            put(x, y, c);
        }
    }

    // ДЕКОР/ВРАГИ-БИЛБОРДЫ в ПОЛНОМ разрешении вида (W, не NW=128): рисуем ПОСЛЕ блита прямо в выход через put,
    // z-тест против стен по ЭКРАННОМУ zbuf[x]=forward-дист (как аппаратные VDP-спрайты оригинала → полный
    // гориз. рез, спрайт семплится per-output-px). screenX/ширина клетки — в W-пространстве (FOV 90°, focal=W/2);
    // высота (faFocalV=64) и горизонт (SH/2) — 1:1 как у стен (верт. разрешение не удваивается).
    // ПИТЧ спрайтов = scale·pitch/64 (per-item, ВНУТРИ drawDecorBillboards, как ROM 1bcaa/стены) — НЕ постоянный
    // viewShiftPx (был неверен: присед двигал далёких/близких одинаково). Спрайт едет с полом на своей дистанции.
    if (faDecorFullRes() && faDecor() && !faWallsOnly() && meta.obj && meta.obj->count > 0)
        drawDecorBillboards(W, H, lvl, cam, *meta.obj, wallPal, ramps, doShade, drawDist,
            [&](int x, int y, uint32_t c) { put(x, y, c); },
            // z-тест: fxZQuant (этап 2) = ЦЕЛЫЕ СКЕЙЛЫ как ROM ef20/167b0 — стена режет колонку спрайта только
            // если её скейл СТРОГО крупнее (ближе); равные скейлы (один D0-бакет) → СПРАЙТ выигрывает. Это
            // ROM-замена float-BIAS 0.40: тот компенсировал непрерывную глубину (боковая стена «вровень» срезала
            // пол-спрайта). ⚠D0-бакет растёт с дистанцией (~f²/64: 0.25кл на f=4, ~1кл на f=8) — вдали спрайт
            // «выигрывает» щедрее BIAS, это ОРИГИНАЛ. Легаси (fxzquant off) = прежний BIAS.
            [&](int x, double f) {
                if (x < 0 || x >= W) return false;
                if (fxZQuant()) {                                     // стена СТРОГО ближе по целому скейлу → режет
                    bool q = (int)std::lround(faFocalV() / zbuf[x]) > fxD0FromPerp(f);
                    static const bool dbg = std::getenv("ZTDBG_ZQ") != nullptr;   // диагностика A/B (как ZTDBG_COL)
                    if (dbg) { static bool once = false; if (!once) { once = true; std::fprintf(stderr, "zq-active\n"); } }
                    if (dbg && q != (zbuf[x] < f - 0.40))
                        std::fprintf(stderr, "zq-diff x=%d f=%.3f zbuf=%.3f wallD0=%d sprD0=%d quant=%d\n",
                                     x, f, zbuf[x], (int)std::lround(faFocalV() / zbuf[x]), fxD0FromPerp(f), q ? 1 : 0);
                    return q;
                }
                const double BIAS = 0.40; return zbuf[x] < f - BIAS;  // ≈полу-ширина спрайта (кл)
            },
            [&](double f, double l) { return W * 0.5 * (1.0 + l / f); },
            [&](double f) { return faFocalV() / f; });
}
