// ztpp — фоны (панорамы неба/города/космоса) для 3D-вида, как в оригинальной ZT.
//
// Механика (по дизасму ZT):
//  - Фон = ПАНОРАМА 1024×128 px (128×16 тайлов 8×8 4bpp), собранная по MD-nametable.
//    Город: gfx 0x14F046, nt 0x153106, base 0x141, пал 0x2132 (флаг -$58e6==0).
//    Космос: gfx 0x154406, nt 0x1553E6, base 0x141, пал 0x2232 (флаг -$58e6==1).
//  - Это отдельный VDP-план ЗА стенами; виден над стенами (в уровнях без потолка) и сквозь
//    прозрачные тексели стен (MD цвет-индекс 0).
//  - Горизонтальный скролл = (−camAngle·2) & 0x3FF  (дизасм 0x1470). camAngle 0..0x1FF (512=360°),
//    панорама 1024 px → 2 px на угловую единицу, 1024 px = 360°.
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include <vector>
#include <cstdint>
#include <cmath>

struct Panorama {
    int w = 0, h = 0;                 // px (1024×128)
    std::vector<uint32_t> px;         // ARGB
    bool valid() const { return w > 0 && !px.empty(); }
    // Сэмпл с гориз. заворотом (панорама циклична на 360°).
    uint32_t sample(int bx, int by) const {
        bx %= w; if (bx < 0) bx += w;
        if (by < 0) by = 0; else if (by >= h) by = h - 1;
        return px[(size_t)by * w + bx];
    }
};

// Декод панорамы: 8×8 4bpp MD-тайлы (gfx) по nametable (W×H слов: idx&0x7FF − base,
// hflip b11, vflip b12), палитра-линия 16 цветов @palAddr (CRAM 3-бит).
inline Panorama decodePanorama(const Rom& rom, size_t gfx, size_t nt,
                               int base, int W, int H, size_t palAddr) {
    Panorama p; p.w = W * 8; p.h = H * 8; p.px.assign((size_t)p.w * p.h, 0xFF000000u);
    Palette pal = readPalette(rom, palAddr);
    for (int ty = 0; ty < H; ++ty)
        for (int tx = 0; tx < W; ++tx) {
            uint16_t e = rom.u16(nt + (size_t)(ty * W + tx) * 2);
            int idx = (e & 0x7FF) - base;
            if (idx < 0) continue;
            size_t taddr = gfx + (size_t)idx * 32;          // 32 б/тайл
            bool hflip = (e & 0x800) != 0, vflip = (e & 0x1000) != 0;
            for (int r = 0; r < 8; ++r)
                for (int c = 0; c < 8; ++c) {
                    uint8_t byte = rom.u8(taddr + r * 4 + (c >> 1));
                    uint8_t pix = (c & 1) ? (byte & 0xF) : (byte >> 4);  // hi-ниббл = левый
                    int dx = tx * 8 + (hflip ? 7 - c : c);
                    int dy = ty * 8 + (vflip ? 7 - r : r);
                    p.px[(size_t)dy * p.w + dx] = pal.c[pix];
                }
        }
    return p;
}

// Два фона ZT-релиза. sel: 0=Город, 1=Космос (флаг -$58e6).
inline Panorama loadZtBackground(const Rom& rom, int sel) {
    if (sel == 1) return decodePanorama(rom, 0x154406, 0x1553E6, 0x141, 128, 16, 0x2232); // космос
    return decodePanorama(rom, 0x14F046, 0x153106, 0x141, 128, 16, 0x2132);              // город
}

// ── ГРАДИЕНТ ПОЛА/ПОТОЛКА (скайбокс-иллюзия) ────────────────────────────────
// ZT рендерит пол/потолок ПО-КОЛОНОЧНО как градиент-backdrop за стенами (дизасм d2d6/d40a:
// потолок над спаном стены, пол под, из буфера-градиента). Имитируем вертикальным градиентом:
// тёмный потолок (верх) → светлая «дымка» к горизонту (середина) → тёмный пол (низ).
// Модуляция яркости по env (0 Bright..4 Black). y∈[0,H), horizon обычно H/2.
inline uint32_t bgGradient(int envMode, int y, int horizon, int H) {
    double t = (double)y / (H > 0 ? H : 1);            // 0 верх .. 1 низ
    double hz = (H > 0) ? (double)horizon / H : 0.5;   // позиция горизонта
    auto mix = [](double a, double b, double k) { return a + (b - a) * k; };
    double topR = 6,  topG = 16, topB = 44;            // потолок (тёмно-синий)
    double horR = 92, horG = 124, horB = 170;          // горизонт (светлая дымка)
    double botR = 26, botG = 20, botB = 14;            // пол (тёмный тёплый)
    double r, g, b;
    if (t < hz) { double k = (hz > 0) ? t / hz : 0;          r = mix(topR, horR, k); g = mix(topG, horG, k); b = mix(topB, horB, k); }
    else        { double k = (hz < 1) ? (t - hz) / (1 - hz) : 0; r = mix(horR, botR, k); g = mix(horG, botG, k); b = mix(horB, botB, k); }
    double m = 1.0; bool gray = false;
    switch (envMode) {
        case 0: m = 1.00; break;          // Bright
        case 1: m = 0.60; break;          // Dim
        case 2: m = 0.85; gray = true; break; // Haze (к серому)
        case 4: m = 0.16; break;          // Black
        default: m = 1.00; break;
    }
    if (gray) { double avg = (r + g + b) / 3.0; r = mix(r, avg, 0.5); g = mix(g, avg, 0.5); b = mix(b, avg, 0.5); }
    r *= m; g *= m; b *= m;
    return 0xFF000000u | ((int)r << 16) | ((int)g << 8) | (int)b;
}

// Активный фон для рендера (null = нет). Ставит main по уровню.
inline const Panorama*& activeBg() { static const Panorama* p = nullptr; return p; }
// Прозрачность тексель-0 → фон активна (env3 + есть фон). Ставит renderFaithful для faDrawSeg.
inline bool& bgSkyTransp() { static bool v = false; return v; }

// Сэмпл фона для экранной колонки x∈[0,W) и пикселя y.
//  - Гориз. (дизасм 0x1470): hscroll = camAngle·2 (угол из dir, 512=360°), окно FOV 256 px (=90°).
//  - Верт. (дизасм 0x14d6): vscroll плана B = floor·4 + cabin/32 − 0x20 → фон сдвигается по ЭТАЖУ
//    (иллюзия высоты: выше этаж — другой срез панорамы) и при переходе лифта (cabin).
// horizon = строка-привязка панорамы (где сидит низ панорамы при floor0); viewH масштабирует.
inline uint32_t bgSample(const Panorama& p, double dirX, double dirY, int floor, double cabin,
                         int x, int W, int y, int horizon) {
    const double TAU = 6.283185307179586;
    double camAngle = std::atan2(dirY, dirX) * (512.0 / TAU);
    double bx = ((double)x - W * 0.5) / W * 256.0 + camAngle * 2.0;
    double vscroll = floor * 4.0 + cabin / 32.0 - 32.0;               // план B vscroll (0x14d6)
    // ПАНОРАМА 1:1 (плоскость B MD НЕ масштабируется — только vscroll): nativeY = строка вида в нат-
    // масштабе (вид 80px, горизонт в центре), by = верх-вьюпорта(40) + nativeY + vscroll. НЕ растягиваем
    // всю панораму (128px) на область потолка — показываем ОКНО, как в игре (иначе фон сжат/искажён).
    double nativeY = (horizon > 0) ? (double)y * 40.0 / horizon : 0.0;
    int by = (int)(40.0 + nativeY + vscroll);          // 40 = верх вьюпорт-окна в кадре HUD (HUD_VY)
    return p.sample((int)std::floor(bx), by);
}
