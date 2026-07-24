// ztpp — src/raycaster.cpp: холодные per-frame функции рейкастера (движение/переходы/спавн).
// НЕ в шаблонах рендера и НЕ per-pixel → безопасно вне заголовка (компилируются один раз, не в каждой TU).
// Горячие хелперы (shade/floorCeil*/bandFor*/rcColumnHitCt/…), предикаты и ШАБЛОНЫ рендера — в raycaster.hpp.
#include "raycaster.hpp"

void rcSpawn(Camera& cam, const Level& lvl) {
    int bx = -1, by = -1;
    // ⭐ROM-спавн (респавн-путь смерти 0x1916-0x1954, VERIFIED 2026-07-24): линейный (row-major) скан
    // ВСЕЙ карты этажа на celltype 0x77 «респавн-камера», позиция = ЦЕНТР клетки (+0x80 в 8.8).
    for (int y = 0; y < Level::H && by < 0; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (cellIcon(lvl.cellType(cam.floor, x, y)) == 8) { bx = x; by = y; break; }
    if (bx < 0) { bx = 8; by = 20; }                // ⭐ФОЛБЭК ROM 0x1938: индекс 0x288 = x8,y20 (была эвристика «открытая клетка»)
    cam.px = bx + 0.5; cam.py = by + 0.5;
    // ⭐УГОЛ = 0 (ВОСТОК, +X): ROM 0x1856 clr -$71FC → вектор из LUT 0x8124[0]. Была эвристика
    // «направление с наибольшей свободой» — НЕфейтфул (после смерти оригинал всегда смотрит на восток).
    cam.dirX = 1; cam.dirY = 0;
    cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66;
    cam.ang512 = 0;
    cam.angI = -1;                                                                  // fixedmove пере-синхр. из новой позы
}

// Лестничный наклон cabin по суб-позиции игрока (ZT @b51a). Возвращает false, если ct — не лестничная клетка.
bool rcStairCabin(Camera& c, uint8_t ct, double subX, double subY) {
    switch (ct) {
        case 0x12: c.cabin = (subY - 255.0) / 4.0; break;   // b612
        case 0x14: c.cabin = (255.0 - subY) / 4.0; break;   // b664
        case 0x3c: c.cabin = (-subY) / 4.0;        break;   // b626
        case 0x3e: c.cabin = ( subY) / 4.0;        break;   // b67a
        case 0x44: c.cabin = (-subX) / 4.0;        break;   // b638
        case 0x46: c.cabin = ( subX) / 4.0;        break;   // b68a
        case 0x4c: c.cabin = (subX - 255.0) / 4.0; break;   // b64a
        case 0x4e: c.cabin = (255.0 - subX) / 4.0; break;   // b69a
        case 0x13: case 0x3d: case 0x45: case 0x4d: c.cabin = 0.0; break;  // площадка пролёта (b65e)
        case 0x15: c.cabin = -64.0; break;                  // interstorey-up (b6b0)
        case 0x16: c.cabin =  64.0; break;                  // interstorey-down (b6b8)
        case 0x17: rcStairTrans(c, subX, true);  break;     // переход, ось X, high (b6c0)
        case 0x3f: rcStairTrans(c, subX, false); break;     // переход, ось X, low  (b6ea)
        case 0x47: rcStairTrans(c, subY, true);  break;     // переход, ось Y, high (b714)
        case 0x4f: rcStairTrans(c, subY, false); break;     // переход, ось Y, low  (b73e)
        default:   return false;
    }
    return true;
}

// ── ROM-ТОЧНЫЙ АВТОМАТ ПЕРЕХОДОВ (faElevZT) ──
bool rcUpdateTransitZT(Camera& c, const Level& lvl, bool enteredNewCell) {
    int cx = (int)std::floor(c.px), cy = (int)std::floor(c.py);
    bool inb = (cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H);
    uint8_t ct = inb ? lvl.cellType(c.floor, cx, cy) : 0;
    double subX = (c.px - cx) * 256.0, subY = (c.py - cy) * 256.0;

    // (A) STEP-ON диспетчер (при СМЕНЕ клетки, e3f8→@e420): старт поездки / стоп на площадке.
    if (enteredNewCell) {
        if      (ctElevUp(ct))   { if (c.cabin == 0.0) c.elevState = +1; }                       // e69e
        else if (ctElevDown(ct)) { if (c.cabin == 0.0) c.elevState = -1; }                       // e6fe
        else if (ctElevUpdn(ct)) { if (c.cabin == 0.0) c.elevState = (c.elevState < 0 ? -1 : +1); } // e75e
        else if (ctElevArea(ct)) { if (c.elevState != 0) c.elevState = (c.elevState < 0 ? -2 : +2); c.cabin = 0.0; } // e7d6
    }

    // (B) PER-FRAME автомат (b4e4 при ct>=0x12): рампа кабины ±4/кадр + своп этажа на пробое ±0x40.
    if (c.elevState == +1) {                                    // UP по зданию: floor--
        if (ctElevDown(ct)) {                                   // b798: доезд к 0 на встречной down-кабине
            c.cabin -= 4.0 * simDt(); if (c.cabin < 0.0) { c.cabin = 0.0; c.elevState = +2; }
        } else {                                                // b768→b7ec: пробой -0x40 → floor--, cabin→+0x40
            c.cabin -= 4.0 * simDt();
            if (c.cabin < -64.0) { c.cabin = 64.0; if (c.floor > 0) c.floor--; }
        }
    } else if (c.elevState == -1) {                             // DOWN по зданию: floor++
        if (ctElevUp(ct)) {                                     // b7da: доезд к 0 на встречной up-кабине
            c.cabin += 4.0 * simDt(); if (c.cabin >= 0.0) { c.cabin = 0.0; c.elevState = -2; }
        } else {                                                // b7aa→b862: пробой +0x40 → floor++, cabin→-0x40
            c.cabin += 4.0 * simDt();
            if (c.cabin > 64.0) { c.cabin = -64.0; if (c.floor < 15) c.floor++; }
        }
    } else {                                                    // не в поездке (0 или ±2)
        if (!rcStairCabin(c, ct, subX, subY)) {
            if      (c.cabin > 0.0) { c.cabin -= 4.0 * simDt(); if (c.cabin < 0.0) c.cabin = 0.0; }
            else if (c.cabin < 0.0) { c.cabin += 4.0 * simDt(); if (c.cabin > 0.0) c.cabin = 0.0; }
        }
    }

    // (C) ЦЕНТРИРОВАНИЕ в кабине (a3ec): при |cabin|>=0x18 суб-позиция клампится в [0x20,0xdf] + гасится скорость.
    if (rcElevCabin(ct) && (c.cabin >= 24.0 || c.cabin <= -24.0)) {
        const double LO = 0x20 / 256.0, HI = 0xDF / 256.0;      // 0.125 .. 0.871
        double fx = c.px - cx, fy = c.py - cy; bool clamped = false;
        if (fx < LO) { c.px = cx + LO; clamped = true; } else if (fx > HI) { c.px = cx + HI; clamped = true; }
        if (fy < LO) { c.py = cy + LO; clamped = true; } else if (fy > HI) { c.py = cy + HI; clamped = true; }
        if (clamped) {
            c.pxI = (int)std::lround(c.px * 256.0); c.pyI = (int)std::lround(c.py * 256.0);
            c.fwdV = c.strafeV = c.turnV = 0; c.fwdVel = c.strafeVel = c.turnVel = 0.0;
        }
    }

    c.pitch = -c.cabin;                                         // baee: viewPitch -= cabin (транзитно)
    return false;                                              // ZT НЕ блокирует движение (только центрирует)
}

// Вызывать КАЖДЫЙ кадр. enteredNewCell=true в кадре, когда игрок вошёл в новую клетку (x,y).
bool rcUpdateTransit(Camera& c, const Level& lvl, bool enteredNewCell) {
    if (faElevZT()) return rcUpdateTransitZT(c, lvl, enteredNewCell);   // ROM-точный автомат (дефолт)
    int cx = (int)c.px, cy = (int)c.py;
    bool inb = (cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H);
    uint8_t ct = inb ? lvl.cellType(c.floor, cx, cy) : 0;
    double subX = (c.px - cx) * 256.0, subY = (c.py - cy) * 256.0;

    // ── ЛИФТ: поездка в процессе ──
    if (c.elevState != 0) {
        int dir = c.elevState;
        if (!rcElevCabin(ct)) {
            c.elevState = 0;                            // сошёл с кабины → cabin сам плавно затухнет до 0
        } else {
            if (c.cabin >= 24.0 || c.cabin <= -24.0) {
                double fx = c.px - cx, fy = c.py - cy;
                if (fx < 0.125) c.px = cx + 0.125; else if (fx > 0.871) c.px = cx + 0.871;
                if (fy < 0.125) c.py = cy + 0.125; else if (fy > 0.871) c.py = cy + 0.871;
            }
            if (dir > 0) {                              // вниз по зданию (floor++)
                c.cabin += 4.0 * simDt();
                if (rcElevArrival(ct, dir)) { if (c.cabin >= 0.0) { c.cabin = 0.0; c.elevState = 0; } }
                else if (c.cabin > 64.0)    { c.cabin = -64.0; if (c.floor < 15) c.floor++; }
            } else {                                    // вверх по зданию (floor--)
                c.cabin -= 4.0 * simDt();
                if (rcElevArrival(ct, dir)) { if (c.cabin <= 0.0) { c.cabin = 0.0; c.elevState = 0; } }
                else if (c.cabin < -64.0)   { c.cabin = 64.0;  if (c.floor > 0)  c.floor--; }
            }
            c.pitch = -c.cabin;
            return true;
        }
    }

    // ── ЛИФТ: старт по входу в клетку-departure ──
    int dep = rcElevDepart(ct);
    if (dep == 0 && (ct == 0x34 || ct == 0x53 || ct == 0x57 || ct == 0x5b)) {  // up/down: по соседу
        if (c.floor < 15 && rcElevCell(lvl.cellType(c.floor + 1, cx, cy))) dep = +1;
        else if (c.floor > 0 && rcElevCell(lvl.cellType(c.floor - 1, cx, cy))) dep = -1;
    }
    if (dep != 0 && enteredNewCell) {
        c.elevState = dep; c.cabin = 0.0; c.px = cx + 0.5; c.py = cy + 0.5; c.pitch = 0.0;
        return true;
    }

    // ── ЛЕСТНИЦЫ: наклон по суб-позиции + своп на клетке-перехода ──
    bool onStair = true;
    switch (ct) {
        case 0x12: c.cabin = (subY - 255.0) / 4.0; break;   // b612
        case 0x14: c.cabin = (255.0 - subY) / 4.0; break;   // b664
        case 0x3c: c.cabin = (-subY) / 4.0;        break;   // b626
        case 0x3e: c.cabin = ( subY) / 4.0;        break;   // b67a
        case 0x44: c.cabin = (-subX) / 4.0;        break;   // b638
        case 0x46: c.cabin = ( subX) / 4.0;        break;   // b68a
        case 0x4c: c.cabin = (subX - 255.0) / 4.0; break;   // b64a
        case 0x4e: c.cabin = (255.0 - subX) / 4.0; break;   // b69a
        case 0x13: case 0x3d: case 0x45: case 0x4d: c.cabin = 0.0; break;  // площадка (b65e)
        case 0x15: c.cabin = -64.0; break;                  // interstorey-up (b6b0)
        case 0x16: c.cabin =  64.0; break;                  // interstorey-down (b6b8)
        case 0x17: rcStairTrans(c, subX, true);  break;     // переход, ось X, high (b6c0)
        case 0x3f: rcStairTrans(c, subX, false); break;     // переход, ось X, low  (b6ea)
        case 0x47: rcStairTrans(c, subY, true);  break;     // переход, ось Y, high (b714)
        case 0x4f: rcStairTrans(c, subY, false); break;     // переход, ось Y, low  (b73e)
        default:   onStair = false; break;
    }
    if (!onStair) {                                          // не на лестнице → питч плавно к 0
        if (c.cabin > 0.0) { c.cabin -= 4.0 * simDt(); if (c.cabin < 0.0) c.cabin = 0.0; }
        else if (c.cabin < 0.0) { c.cabin += 4.0 * simDt(); if (c.cabin > 0.0) c.cabin = 0.0; }
    }
    c.pitch = -c.cabin;
    return false;
}

// Полный шаг движения с ИНЕРЦИЕЙ (DAB8). Явные намерения ввода (уже разрешённые из раскладки клавиш).
void rcMovePhysics(Camera& c, const Level& lvl, bool fwd, bool back, bool strafeL, bool strafeR,
                   bool turnL, bool turnR, bool runMod, bool halveMove, bool noclip) {
    if (faFixedMove() && zAngLUTok()) {
        // ══ ЦЕЛОЧИСЛЕННОЕ ДВИЖЕНИЕ как МД (DAB8): угол/позиция/скорости целые → траектория бит-в-бит ══
        if (c.angI < 0) {                                                       // синхр. из float (спавн/переключение)
            c.pxI = (int)std::lround(c.px * 256.0); c.pyI = (int)std::lround(c.py * 256.0);
            c.angI = ((int)std::lround(camDirToAng512(c)) + 128) & 0x1FF;       // порт ang512 (+128 = МД-угол, a0=−Y)
            c.turnV = c.fwdV = c.strafeV = 0;
        }
        int turnT   = (turnR ? 16 : 0) - (turnL ? 16 : 0);                      // цель поворота ±16 (МД -$7180)
        int fwdT    = fwd ? (runMod ? 45 : 40) : (back ? (runMod ? -35 : -20) : 0);  // вперёд/назад (-$717c)
        int strafeT = (strafeL ? 40 : 0) - (strafeR ? 40 : 0);                  // стрейф ±40 (-$7178)
        if (halveMove) { fwdT >>= 1; strafeT >>= 1; }                           // присед/нокдаун ÷2 (asr)
        auto easeMD = [](int vel, int tgt, int accelSh) -> int {                // инерция МД (asr-усечение)
            int sh = (tgt != 0) ? accelSh : 1; int d = tgt - vel;
            if (d > 0) vel -= ((-d) >> sh); else if (d < 0) vel += (d >> sh);
            return vel;
        };
        c.turnV   = easeMD(c.turnV,   turnT,   3);                              // поворот ÷8
        c.fwdV    = easeMD(c.fwdV,    fwdT,    4);                              // вперёд  ÷16
        c.strafeV = easeMD(c.strafeV, strafeT, 4);                              // стрейф  ÷16
        c.angI = (c.angI + c.turnV) & 0x1FF;                                     // угол += turnVel, andi #$1ff (целый!)
        int cs = zAngLUT()[c.angI * 2], sn = zAngLUT()[c.angI * 2 + 1];         // cos/sin ×256 (LUT 0x8124)
        int dx = (cs * c.fwdV) >> 8, dy = (sn * c.fwdV) >> 8;                   // вектор вперёд (>>8 ОТДЕЛЬНО на терм, как МД)
        if (c.strafeV != 0) { dx += (-sn * c.strafeV) >> 8; dy += (cs * c.strafeV) >> 8; }  // + стрейф (−sin,cos)
        if (noclip) { c.pxI += dx; c.pyI += dy; }
        else {                                                                  // коллизия по осям (порт, r=0.18) на 8.8-позиции
            int nxI = c.pxI + dx; double ny0 = c.pyI / 256.0;
            if (dx == 0 || !rcBlockedPt(lvl, c.floor, nxI / 256.0 + (dx > 0 ? 0.18 : -0.18), ny0)) c.pxI = nxI;
            int nyI = c.pyI + dy; double px0 = c.pxI / 256.0;
            if (dy == 0 || !rcBlockedPt(lvl, c.floor, px0, nyI / 256.0 + (dy > 0 ? 0.18 : -0.18))) c.pyI = nyI;
        }
        c.px = c.pxI / 256.0; c.py = c.pyI / 256.0;                             // ВЫВЕСТИ float из целого (для рендера/остального)
        c.dirX = cs / 256.0; c.dirY = sn / 256.0;
        c.planeX = -c.dirY * 0.66; c.planeY = c.dirX * 0.66;
        c.ang512 = (double)((c.angI - 128) & 0x1FF);
        return;
    }
    using namespace ztmove;
    c.angI = -1;                                                                // float-путь: инвалидируем целое (fixedmove пере-синхр.)
    double fwdT = 0.0, strafeT = 0.0, turnT = 0.0;
    if (strafeL) strafeT += STRAFE;                   // влево (+ = вектор «влево» sx)
    if (strafeR) strafeT -= STRAFE;
    if (turnL)   turnT   -= TURN;
    if (turnR)   turnT   += TURN;
    if (fwd)  fwdT = runMod ? FWD_RUN : FWD;
    if (back) fwdT = runMod ? BACK_RUN : BACK;        // назад вдвое медленнее вперёда (40 vs 20)
    if (halveMove) { fwdT *= 0.5; strafeT *= 0.5; }   // присед/нокдаун ÷2
    c.turnVel   = ztEase(c.turnVel,   turnT,   1.0 / 8.0);   // поворот: разгон ÷8
    c.fwdVel    = ztEase(c.fwdVel,    fwdT,    1.0 / 16.0);  // вперёд:  разгон ÷16
    c.strafeVel = ztEase(c.strafeVel, strafeT, 1.0 / 16.0);  // стрейф:  разгон ÷16
    if (c.turnVel != 0.0) {
        if (faAngle512()) camSetAngleU(c, c.ang512 + c.turnVel * (512.0 / PI2));  // квант. 512 шагов (ROM)
        else { rcRotate(c, c.turnVel); c.ang512 = camDirToAng512(c); }           // непрерывно (+ синхр. аккумулятор)
    }
    const double sx = c.dirY, sy = -c.dirX;                  // единичный вектор «ВЛЕВО»
    double dx = c.dirX * c.fwdVel + sx * c.strafeVel;
    double dy = c.dirY * c.fwdVel + sy * c.strafeVel;
    if (dx != 0.0 || dy != 0.0) rcMove(c, lvl, dx, dy, noclip);
}
