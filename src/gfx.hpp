// ztpp — графика: палитра CRAM->ARGB и декод тайла 32x32 4bpp column-major.
#pragma once
#include "rom.hpp"
#include <array>
#include <cstdint>

// Mega Drive CRAM-слово (0000 BBB0 GGG0 RRR0): 3-битные R/G/B -> ARGB8888.
inline uint32_t cramToArgb(uint16_t w) {
    uint8_t r = (w & 0x00E) >> 1;
    uint8_t g = (w & 0x0E0) >> 5;
    uint8_t b = (w & 0xE00) >> 9;
    auto sc = [](uint8_t v) -> uint32_t { return static_cast<uint32_t>((v * 255) / 7); };
    return 0xFF000000u | (sc(r) << 16) | (sc(g) << 8) | sc(b);
}

struct Palette {
    std::array<uint32_t, 16> c{};
};

inline Palette readPalette(const Rom& rom, size_t off) {
    Palette p;
    for (int i = 0; i < 16; ++i) p.c[i] = cramToArgb(rom.u16(off + i * 2));
    return p;
}

// Декод тайла ZT: 32x32, 4bpp, COLUMN-MAJOR, 512 байт (как mdgfx.tiles.decode_zt_sheet).
// Раскладка: 16 ПОЛОС по 2 пикселя в ширину, 32 байта на полосу (1 байт = строка полосы).
// Полоса col покрывает выходные колонки 2*col и 2*col+1; в байте старший ниббл = левый
// пиксель (x=2*col), младший = правый (x=2*col+1). Выход: out[y*32 + x] = индекс палитры.
inline void decodeTile(const Rom& rom, size_t off, uint8_t out[32 * 32]) {
    for (int col = 0; col < 16; ++col) {
        size_t coff = off + col * 32;
        int x = col * 2;
        for (int y = 0; y < 32; ++y) {
            uint8_t byte = rom.u8(coff + y);
            out[y * 32 + x]     = byte >> 4;
            out[y * 32 + x + 1] = byte & 0x0F;
        }
    }
}
