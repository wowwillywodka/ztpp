#pragma once
// ── SEGA-ЗАСТАВКА ZT — ВРАЩАЮЩИЙСЯ ЛОГОТИП (VERIFIED дизасм sub_05711e, вызов из boot 0xA2C ПЕРЕД копирайтом) ──
// Механика ROM: логотип = «стена» игрового движка (рендер ccb6) с текстурой SEGA из БАНКА СТЕН
// (тайлы 0xF7..0xFE, лист 128×64: профиль @0x5710E = пары (верх,низ) × 4 колонны по 32px).
// Геометрия: два ребра листа через sin-LUT 0x8124 (512 записей, w0=sin·256, w2=−cos·256):
//   idx = θ & 0x1FF;  d = w2 + dist;  X = (((sin<<8)/0xB5)<<6)/d + 0x40;  scale = 0x8000/d
// Кадр: palStep++ (mod 22); θ −= 8 (wrap ((θ+0xC0)&0x7F)−0xC0); dist −= max(0x18,(dist−0x2B5)>>4) до 0x2B5;
// грань1 = рёбра (θ, θ+0x80), грань2 = (θ+0x80, θ+0x100); грань видима при X_l < X_r (backface-cull).
// Сходимость: θ==−0xC0 && dist==0x2B5 (грань2 даёт X 0x20..0x60, scale 0x40 = лист 128×64 анфас 1:1 —
// совпадает с константами финального блока 574E2). Затем фаза 2: ≥16 кадров статично + допалить палитро-цикл
// до palStep==0, затем 30 кадров паузы (без анимации) → RTS → копирайт.
// Палитра: CRAM 0..15 = @0x570C2 (чёрный, белый, ч, ч) + окно 12 слов кольца @0x570CA (22 сдвига) в цвета 4..15.
// Окно вывода: NT 32×10 тайлов @0xC488 = экран 256×80 @ (32,72); колонка листа = 2 экранных px (128 колонн).
#include <cstdint>
#include <cmath>
#include <vector>
#include "rom/gamedata.hpp"
#include "rom/gfx.hpp"

namespace segai {

constexpr int WIN_X = 32, WIN_Y = 72, WIN_W = 256, WIN_H = 80;   // NT-окно 0xC488
constexpr int TEX_W = 128, TEX_H = 64;

// состояние анимации на кадр F (детерминированная прокрутка ROM-цикла 57200).
// ⭐ТЕМП (VERIFIED MAME-трейсом $FF0876/$FF0874 2026-07-18): цикл ждёт 2 VBlank (`move #2,-$7ffe` перед
// 1a34), а рендер ccb6 крупного листа сам съедает 3-й кадр → шаг = 2 кадра, с dist≲1130 — 3 кадра;
// статик-фаза (лист анфас) = 3 кадра/шаг; пауза 5754e = 30 VBlank. Итого ≈263 кадра ≈ 4.4 с.
struct St { int theta, dist, pal; int phase; };   // phase: 0=вращение, 1=статик+палитра, 2=пауза, 3=конец
inline int wrapTheta(int t) { return ((t + 0xC0) & 0x7F) - 0xC0; }
inline St stateAt(int frame) {
    St s{0, 0xE00, 0, 0};
    int extra = -1;                                    // фаза 1: счётчик 16 повторов + до-цикл палитры; фаза 2: пауза
    int f = 0;
    while (s.phase < 3) {
        int dur = (s.phase == 0) ? (s.dist >= 1130 ? 2 : 3)   // шаг вращения: 2 VBlank, крупный лист — 3
                : (s.phase == 1) ? 3 : 1;                      // статик 3 кадра/шаг; пауза покадрово
        if (f + dur > frame) break;                            // кадр frame внутри показа этого состояния
        f += dur;
        if (s.phase == 0) {
            s.pal = (s.pal + 1) % 22;
            s.theta = wrapTheta(s.theta - 8);
            int step = (s.dist - 0x2B5) >> 4; if (step < 0x18) step = 0x18;
            s.dist -= step; if (s.dist < 0x2B5) s.dist = 0x2B5;
            if (s.theta == -0xC0 && s.dist == 0x2B5) { s.phase = 1; extra = 16; }
        } else if (s.phase == 1) {
            s.pal = (s.pal + 1) % 22;
            if (extra > 0) --extra;
            if (extra == 0 && s.pal == 0) { s.phase = 2; extra = 30; }   // 5754e: 0x1E кадров паузы
        } else if (s.phase == 2) {
            if (--extra <= 0) s.phase = 3;
        }
    }
    return s;
}
// полная длительность заставки в кадрах 60Гц (до phase 3)
inline int totalFrames() { for (int f = 1; f < 2000; ++f) if (stateAt(f).phase == 3) return f; return 300; }

// тексель листа: u 0..127, v 0..63 → индекс палитры. Текстура = wall-тайлы 0xF7..0xFE, лист 4×2 тайла
// (верх 0xF7..0xFA, низ 0xFB..0xFE; профиль 0x5710E перечисляет пары (верх,низ) колонн). Формат тайла
// стены ZT (WallBank::decode): ПАРА колонок × 32 строки: byte = data[tile·512 + (x/2)·32 + y],
// левая колонка = верхний ниббл, правая = нижний.
inline uint8_t texAt(const GameData& gd, int u, int v) {
    int col = (u >> 5) & 3, sub = u & 31;
    int tile = (v < 32) ? (0xF7 + col) : (0xFB + col);
    int row = v & 31;
    size_t off = (size_t)tile * 512 + (size_t)(sub >> 1) * 32 + (size_t)row;
    if (off >= gd.wall.data.size()) return 0;
    uint8_t b = gd.wall.data[off];
    return (sub & 1) ? (b & 0x0F) : (uint8_t)(b >> 4);
}

// Рендер кадра F (320×224 ARGB). Возврат false = заставка закончилась.
inline bool renderSegaFrame(const GameData& gd, int frame, uint32_t* out) {
    if (!gd.hasSega || gd.wall.data.empty()) return false;
    St s = stateAt(frame);
    if (s.phase >= 3) return false;
    // палитра кадра: base 0..3 + окно кольца в 4..15 (ROM 57212: CRAM @0x08 = цвет 4)
    uint32_t pal[16];
    for (int i = 0; i < 4; ++i)  pal[i] = cramToArgb(gd.segaPal[i]);
    for (int i = 0; i < 12; ++i) pal[4 + i] = cramToArgb(gd.segaAnim[s.pal + i]);
    for (size_t i = 0; i < (size_t)320 * 224; ++i) out[i] = 0xFF000000u;
    // рёбра листа по LUT (0x8124 в ROM; здесь sin/cos тем же квантованием: w0=sin·256, w2=−cos·256)
    auto edge = [&](int th, int& X, int& sc) {
        int idx = th & 0x1FF;
        double a = idx * 6.283185307179586 / 512.0;
        int sn = (int)std::lround(std::sin(a) * 256.0);
        int cs = (int)std::lround(-std::cos(a) * 256.0);
        int d = cs + s.dist; if (d < 1) d = 1;
        X  = (((sn << 8) / 0xB5) << 6) / d + 0x40;
        sc = 0x8000 / d;
    };
    // грань: рёбра (thL, thR), текстура u 0..127 слева-направо; невидима при Xl >= Xr (backface, ROM ccb6)
    auto face = [&](int thL, int thR) {
        int xl, xr, scl, scr;
        edge(thL, xl, scl); edge(thR, xr, scr);
        if (xl >= xr) return;
        for (int cx = xl; cx <= xr; ++cx) {
            if (cx < 0 || cx > 127) continue;
            double t = (xr > xl) ? (double)(cx - xl) / (double)(xr - xl) : 0.0;
            int sc = (int)(scl + (scr - scl) * t + 0.5);            // аффинная интерп (как ccb6/стены ZT)
            if (sc < 1) continue;
            // ⭐STRIP-TEXEL (как стены ZT): колонка экрана 2px = ДВА соседних текселя (лев u, прав u+1)
            int u = ((int)(t * (TEX_W - 2) + 0.5)) & ~1;
            int h = sc; if (h > WIN_H) h = WIN_H;                    // высота колонны: scale px (0x40=64=1:1)
            int y0 = WIN_H / 2 - h / 2;
            for (int py = 0; py < h; ++py) {
                int v = (sc > WIN_H) ? (int)((double)(py + (sc - WIN_H) / 2) * TEX_H / sc)
                                     : (int)((double)py * TEX_H / sc);   // центрировка при клипе к окну
                if (v < 0) v = 0; if (v > TEX_H - 1) v = TEX_H - 1;
                int sy = WIN_Y + y0 + py;
                int sx = WIN_X + cx * 2;                             // колонка = 2 px (128 колонн на 256)
                if (sy < 0 || sy >= 224) continue;
                if (sx >= 0 && sx < 319) {
                    out[(size_t)sy * 320 + sx]     = pal[texAt(gd, u, v)];
                    out[(size_t)sy * 320 + sx + 1] = pal[texAt(gd, u + 1, v)];
                }
            }
        }
    };
    if (s.phase == 0) { face(s.theta, s.theta + 0x80); face(s.theta + 0x80, s.theta + 0x100); }
    else              { face(-0xC0 + 0x80, -0xC0 + 0x100); }   // статик: грань2 финала (X 0x20..0x60, scale 0x40)
    return true;
}

} // namespace segai
