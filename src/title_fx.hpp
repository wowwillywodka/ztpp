#pragma once
// ── ТИТУЛЬНЫЙ ЭКРАН ZT — полный рендер анимации (VERIFIED дизасм 1cf36/1d4f6/1d7d2, цикл 688 кадров @60Гц) ──
// Верх (тайл-строки 0..11): бегущая лента «ZERO TOLERANCE» — колонка из 12 тайлов NT @0x119B8E (строки по
// 0x100 байт) пишется каждые 4 кадра в dst=((2k+0x54)&0xFF)/2 плана A, слово+1; hscroll = −2px/кадр.
// Низ (строки 13..27): санбёрст-лучи NT @0x11C576 (128×15 слов) ДВУМЯ копиями — план A (+0x4090, пал.2) и
// план B (+0x6090, пал.3), тайл-база +0x90; пер-СТРОЧНЫЙ скос hscroll: acc=−0x1620−d·(i+1), слово=(acc>>4)
// &~1 (A) / |1 (B), d=clamp(ctr,0..0x3F)−0x20. ПУЛЬС: стримы длительностей @0x1D584 (A) / 0x1D594 (B) →
// счётчик ctr по сегменту; v=clamp(|min(ctr,0x40)−0x20|,0,0x1F); CRAM line2/3 = 0x1DCD4 − v·0x20 (32
// градации @0x1D8F4..0x1DCF3). CRAM line1 = ГЛОУ силуэта (sub_01d7d2): 5 слотов @0x1D692[ctr·5], каждый →
// триплет цветов @0x1D622[(b−1)·8]. line0 статична @0x1DCF4. Спрайты: силуэт бойца 16 SAT-записей @0x1D5A2
// (слои глоу пал.1 + статик пал.0), меню START/OPTIONS 4 @0x1D4D6, курсор tile 0x40B (Y=(sel−1)·24+0x112).
// Тайлы: bank1 0x8F @0x11B38E (VRAM 0), bank2 0x2C5 @0x11DE76 (VRAM 0x8F), спрайт-тайлы 182 @0x123716 +
// 26 @0x124DD6 (VRAM 0x355).
#include <cstdint>
#include <vector>
#include <algorithm>
#include "rom/gamedata.hpp"
#include "rom/gfx.hpp"

namespace ttl {
constexpr size_t GFX_BASE = 0x119B8E;   // gd.titleGfxBlk = ROM [0x119B8E..0x125100)
constexpr size_t TAB_BASE = 0x1D400;    // gd.titleTabBlk = ROM [0x1D400..0x1DE00)
constexpr int    CYCLE    = 688;        // цикл ленты (рестарт 1d472)

inline uint16_t gw(const std::vector<uint8_t>& b, size_t romAddr, size_t base) {
    size_t off = romAddr - base;
    return (off + 1 < b.size()) ? (uint16_t)((b[off] << 8) | b[off + 1]) : 0;
}
inline uint8_t gb(const std::vector<uint8_t>& b, size_t romAddr, size_t base) {
    size_t off = romAddr - base; return off < b.size() ? b[off] : 0;
}

// Рендер кадра F титула в out (320×224 ARGB). menuSel: 0=меню скрыто, 1=START, 2=OPTIONS.
inline void renderTitleFrame(const GameData& gd, int frame, int menuSel, uint32_t* out) {
    const auto& G = gd.titleGfxBlk; const auto& T = gd.titleTabBlk;
    if (G.empty() || T.empty()) return;
    int F = frame % CYCLE;
    // счётчик пульса: стрим = слова-длительности сегментов; внутри сегмента v счётчик идёт v-1..0
    auto ctrAt = [&](size_t stream, int f) {
        size_t p = stream;
        for (int guard = 0; guard < 64; ++guard) {
            int v = gw(T, p, TAB_BASE); p += 2;
            if (v <= 0) return 0;
            if (f < v) return v - 1 - f;
            f -= v;
        }
        return 0;
    };
    int ctrA = ctrAt(0x1D584, F), ctrB = ctrAt(0x1D594, F);
    auto wv = [](int c) { int m = c < 0x40 ? c : 0x40; int a = m - 0x20; if (a < 0) a = -a; return a > 0x1F ? 0x1F : a; };
    uint32_t pal[4][16];
    for (int i = 0; i < 16; ++i) pal[0][i] = cramToArgb(gw(T, 0x1DCF4 + (size_t)i * 2, TAB_BASE));
    for (int i = 0; i < 16; ++i) pal[2][i] = cramToArgb(gw(T, 0x1DCD4 - (size_t)wv(ctrA) * 0x20 + (size_t)i * 2, TAB_BASE));
    for (int i = 0; i < 16; ++i) pal[3][i] = cramToArgb(gw(T, 0x1DCD4 - (size_t)wv(ctrB) * 0x20 + (size_t)i * 2, TAB_BASE));
    // ⭐ГЛОУ line1 (1d7d2, перечитан 2026-07-18): слот s → цвета (13−3s, 14−3s, 15−3s) — слот 0 = 13..15,
    // слот 4 = 1..3 (ОБРАТНЫЙ порядок; прежний код клал слот 0 в 1..3 → «битая» палитра глоу).
    // Сначала ctrA-проход, затем ctrB ПОВЕРХ (те же приёмники; слот с байтом 0 не пишется).
    uint16_t l1[16] = {0};
    for (int c : {ctrA, ctrB}) if (c >= 0 && c < 0x40)
        for (int s = 0; s < 5; ++s) {
            int b = gb(T, 0x1D692 + (size_t)c * 5 + s, TAB_BASE);
            if (b) for (int k = 0; k < 3; ++k) l1[13 - s * 3 + k] = gw(T, 0x1D622 + (size_t)(b - 1) * 8 + (size_t)k * 2, TAB_BASE);
        }
    for (int i = 0; i < 16; ++i) pal[1][i] = cramToArgb(l1[i]);
    // планы A/B 128×32 тайл-слов
    static std::vector<uint16_t> pA, pB;
    pA.assign(128 * 32, 0); pB.assign(128 * 32, 0);
    // ⭐лента пишется в PLANE B (1d1d8: dst = 0xE000+((d2+0x54)&0xFF), НЕ Plane A; строки 0..11 Plane B)
    int cols = (F >= 3) ? (F - 3) / 4 + 1 : 0;                          // колонка-стример: 1 колонка / 4 кадра
    for (int k = 0; k < cols; ++k) {
        int d2 = 2 * k; int dst = ((d2 + 0x54) & 0xFF) / 2;
        for (int r = 0; r < 12; ++r) {
            uint16_t w = gw(G, 0x119B8E + (size_t)d2 + (size_t)r * 0x100, GFX_BASE);
            pB[(size_t)r * 128 + dst] = (d2 < 0x100) ? (uint16_t)(w + 1) : 0;   // после 128 колонок — нули (стирание)
        }
    }
    for (int i = 0; i < 1920; ++i) {                                    // лучи (статичный NT, скос — hscroll'ом)
        uint16_t w = gw(G, 0x11C576 + (size_t)i * 2, GFX_BASE);
        pA[(size_t)(13 + i / 128) * 128 + i % 128] = (uint16_t)(w + 0x4090);
        pB[(size_t)(13 + i / 128) * 128 + i % 128] = (uint16_t)(w + 0x6090);
    }
    auto tilePx = [&](int idx, int px, int py) -> int {                 // ниббл (x,y) тайла VRAM-индекса idx
        // ⭐VRAM-раскладка (1d020/1d048): тайл 0 = НУЛИ (8 лонгов), банки грузятся С ИНДЕКСА 1:
        // bank1 (заголовок 0x119A86: 0x8F тайлов @0x11B38E) = VRAM 1..0x8F; bank2 (0x11C56E: 0x2C5
        // @0x11DE76) = VRAM 0x90..0x354. Прежний код брал bank1[idx] без −1 → ВСЯ лента/лучи смещены
        // на один тайл («битая графика»). Спрайт-тайлы 0x355+ грузятся отдельно (1d0c2) — без сдвига.
        size_t addr;
        if (idx <= 0)          return 0;                                // тайл 0 — пустой
        else if (idx <= 0x8F)  addr = 0x11B38E + (size_t)(idx - 1) * 32;
        else if (idx <= 0x354) addr = 0x11DE76 + (size_t)(idx - 0x90) * 32;
        else { int s = idx - 0x355; if (s >= 182 + 26) return 0;
               addr = (s < 182) ? 0x123716 + (size_t)s * 32 : 0x124DD6 + (size_t)(s - 182) * 32; }
        uint8_t b = gb(G, addr + (size_t)py * 4 + px / 2, GFX_BASE);
        return (px & 1) ? (b & 0xF) : (b >> 4);
    };
    for (int sy = 0; sy < 224; ++sy) {                                  // фоновые планы (B под A)
        int ha, hb;
        if (sy < 104) { ha = hb = (int16_t)((-2 * (F + 1)) & 0xFFFF); } // лента: общий hscroll −2px/кадр
        else {
            int i = sy - 104;
            int d0 = (ctrA < 0x3F ? ctrA : 0x3F) - 0x20, d4 = (ctrB < 0x3F ? ctrB : 0x3F) - 0x20;
            ha = (int16_t)(((-0x1620 - d0 * (i + 1)) >> 4) & ~1);       // скос лучей per-line (чёт/нечёт бит
            hb = (int16_t)(((-0x1620 - d4 * (i + 1)) >> 4) | 1);        //   разводит копии A и B на 1px)
        }
        for (int sx = 0; sx < 320; ++sx) {
            uint32_t col = 0xFF000000u;
            const std::vector<uint16_t>* pls[2] = { &pB, &pA }; int hs[2] = { hb, ha };
            for (int li = 0; li < 2; ++li) {
                int u = ((sx - hs[li]) % 1024 + 1024) % 1024;
                uint16_t a = (*pls[li])[(size_t)(sy >> 3) * 128 + (u >> 3)];
                if (!a) continue;
                int px = u & 7, py = sy & 7;
                if (a & 0x800)  px = 7 - px;
                if (a & 0x1000) py = 7 - py;
                int c = tilePx(a & 0x7FF, px, py);
                if (c) col = pal[(a >> 13) & 3][c];
            }
            out[(size_t)sy * 320 + sx] = col;
        }
    }
    // спрайты: силуэт (16, слой глоу пал.1 + статик пал.0) [+ курсор + START/OPTIONS при меню]
    struct S { int y, sz, at, x; };
    std::vector<S> spr;
    for (int i = 0; i < 16; ++i)
        spr.push_back({ gw(T, 0x1D5A2 + (size_t)i * 8, TAB_BASE), gw(T, 0x1D5A2 + (size_t)i * 8 + 2, TAB_BASE),
                        gw(T, 0x1D5A2 + (size_t)i * 8 + 4, TAB_BASE), gw(T, 0x1D5A2 + (size_t)i * 8 + 6, TAB_BASE) });
    if (menuSel > 0) {
        spr.push_back({ (menuSel - 1) * 24 + 0x112, 0x0400, 0x840B, 0xFC });   // курсор-стрелка (1d4f6)
        for (int i = 0; i < 4; ++i)
            spr.push_back({ gw(T, 0x1D4D6 + (size_t)i * 8, TAB_BASE), gw(T, 0x1D4D6 + (size_t)i * 8 + 2, TAB_BASE),
                            gw(T, 0x1D4D6 + (size_t)i * 8 + 4, TAB_BASE), gw(T, 0x1D4D6 + (size_t)i * 8 + 6, TAB_BASE) });
    }
    for (int si = (int)spr.size() - 1; si >= 0; --si) {                 // MD: меньший индекс = выше приоритет
        const S& s = spr[si];
        int wt = ((s.sz >> 10) & 3) + 1, ht = ((s.sz >> 8) & 3) + 1;
        const uint32_t* p = pal[(s.at >> 13) & 3];
        for (int cx = 0; cx < wt; ++cx)
            for (int cy = 0; cy < ht; ++cy) {
                int ti = (s.at & 0x7FF) + cx * ht + cy;
                for (int yy = 0; yy < 8; ++yy)
                    for (int xx = 0; xx < 8; ++xx) {
                        int c = tilePx(ti, xx, yy);
                        if (!c) continue;
                        int sxp = s.x - 128 + cx * 8 + xx, syp = s.y - 128 + cy * 8 + yy;
                        if (sxp >= 0 && sxp < 320 && syp >= 0 && syp < 224) out[(size_t)syp * 320 + sxp] = p[c];
                    }
            }
    }
}
} // namespace ttl
