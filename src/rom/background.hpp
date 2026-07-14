// ztpp — фоны-панорамы (небо/город/космос): ДЕКОД из ROM. Рантайм-хелперы рендера
// (скролл/градиент/сэмпл по камере) вынесены в src/render/background_fx.hpp.
//
// Механика (по дизасму ZT):
//  - Фон = ПАНОРАМА 1024×128 px (128×16 тайлов 8×8 4bpp), собранная по MD-nametable.
//    Город: gfx 0x14F046, nt 0x153106, base 0x141, пал 0x2132 (флаг -$58e6==0).
//    Космос: gfx 0x154406, nt 0x1553E6, base 0x141, пал 0x2232 (флаг -$58e6==1).
//  - Это отдельный VDP-план ЗА стенами; виден над стенами (в уровнях без потолка) и сквозь
//    прозрачные тексели стен (MD цвет-индекс 0).
#pragma once
#include "rom.hpp"
#include <vector>
#include <cstdint>

struct Panorama {
    int w = 0, h = 0;                 // px (1024×128)
    std::vector<uint32_t> px;         // ARGB
    bool valid() const { return w > 0 && !px.empty(); }
    // Сэмпл с гориз. заворотом (панорама циклична на 360°). Горячий — inline.
    uint32_t sample(int bx, int by) const {
        bx %= w; if (bx < 0) bx += w;
        if (by < 0) by = 0; else if (by >= h) by = h - 1;
        return px[(size_t)by * w + bx];
    }
};

// Декод панорамы: 8×8 4bpp MD-тайлы (gfx) по nametable (W×H слов: idx&0x7FF − base,
// hflip b11, vflip b12), палитра-линия 16 цветов @palAddr (CRAM 3-бит).  → background.cpp
Panorama decodePanorama(const Rom& rom, size_t gfx, size_t nt, int base, int W, int H, size_t palAddr);

// Два фона ZT-релиза. sel: 0=Город, 1=Космос (флаг -$58e6).  → background.cpp
Panorama loadZtBackground(const Rom& rom, int sel);
