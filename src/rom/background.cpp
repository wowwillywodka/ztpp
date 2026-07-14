// ztpp — src/rom/background.cpp: декод панорам из ROM (раз на билд, не горячий путь).
#include "background.hpp"
#include "gfx.hpp"

Panorama decodePanorama(const Rom& rom, size_t gfx, size_t nt,
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

Panorama loadZtBackground(const Rom& rom, int sel) {
    if (sel == 1) return decodePanorama(rom, 0x154406, 0x1553E6, 0x141, 128, 16, 0x2232); // космос
    return decodePanorama(rom, 0x14F046, 0x153106, 0x141, 128, 16, 0x2132);              // город
}
