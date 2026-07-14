// ztpp — src/render/background_fx.hpp: РАНТАЙМ-хелперы фона (скролл по камере, градиент пол/потолок,
// активная панорама). Декод панорам — в src/rom/background.hpp. Функции горячие (на колонку кадра)
// либо синглтоны → остаются inline в заголовке.
#pragma once
#include "background.hpp"   // Panorama (декодер-слой)
#include <cstdint>
#include <cmath>

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

// Сэмпл фона для экранной колонки x∈[0,W) и пикселя y.  Горячий (на колонку) → inline.
//  - Гориз. (дизасм 0x1470): hscroll = camAngle·2 (угол из dir, 512=360°), окно FOV 256 px (=90°).
//  - Верт. (дизасм 0x14d6): vscroll плана B = floor·4 + cabin/32 − 0x20 → фон сдвигается по ЭТАЖУ
//    (иллюзия высоты) и при переходе лифта (cabin).
// horizon = строка-привязка панорамы (где сидит низ панорамы при floor0); viewH масштабирует.
inline uint32_t bgSample(const Panorama& p, double dirX, double dirY, int floor, double cabin,
                         int x, int W, int y, int horizon) {
    const double TAU = 6.283185307179586;
    double camAngle = std::atan2(dirY, dirX) * (512.0 / TAU);
    double bx = ((double)x - W * 0.5) / W * 256.0 + camAngle * 2.0;
    double vscroll = floor * 4.0 + cabin / 32.0 - 32.0;               // план B vscroll (0x14d6)
    double nativeY = (horizon > 0) ? (double)y * 40.0 / horizon : 0.0;
    int by = (int)(40.0 + nativeY + vscroll);          // 40 = верх вьюпорт-окна в кадре HUD (HUD_VY)
    return p.sample((int)std::floor(bx), by);
}
