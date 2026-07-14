// ztpp — src/render/framebuffer.hpp: софт-framebuffer порта + размеры вьюпорта.
// Вынесено из main.cpp (общий тип для рендеров). FBW/FBH/g_view* — C++17 inline-переменные
// (одна копия на все TU; задаются в main до создания FB/окна). FB::put — горячий, inline.
#pragma once
#include <vector>
#include <cstdint>
#include <algorithm>

// FBW/FBH = 640×render_scale; задаются в main до создания FB/окна.
inline int FBW = 640, FBH = 640;
// Регион ИГРЫ внутри fb (для аспект-корректного блита в окно — вариант A). Reference = 640×448 (320×224 ×2),
// прочие режимы = весь fb. Презентуем ВЕСЬ fb, но dest-аспект считаем так, чтобы ИГРА была верной пропорции.
inline int g_viewX = 0, g_viewY = 0, g_viewW = FBW, g_viewH = FBH;

struct FB {
    std::vector<uint32_t> px;
    FB() : px(static_cast<size_t>(FBW) * FBH, 0xFF000000u) {}
    void clear(uint32_t c = 0xFF101014u) { std::fill(px.begin(), px.end(), c); }
    inline void put(int x, int y, uint32_t c) {
        if (x >= 0 && x < FBW && y >= 0 && y < FBH) px[static_cast<size_t>(y) * FBW + x] = c;
    }
    void rect(int x0, int y0, int w, int h, uint32_t c) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) put(x, y, c);
    }
};
