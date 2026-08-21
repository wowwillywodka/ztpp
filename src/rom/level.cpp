// ztpp — src/rom/level.cpp: загрузчики уровня ZMAP (раз на эпизод, не горячий путь).
#include "level.hpp"

void parseWallAnims(const Rom& rom, size_t sig, size_t animTableOff,
                    std::vector<WallAnim>& out) {
    out.clear();
    if (rom.u32(sig) != 0x5A4D4150u) return;               // "ZMAP"
    size_t p = sig + animTableOff;
    size_t n = rom.size();
    while (p + 6 <= n) {
        uint16_t stride = rom.u16(p);
        if (stride < 6 || stride > 0x800) break;           // stride==0 → терминатор таблицы
        if (p + stride > n) break;
        WallAnim an; an.period = rom.u8(p + 3); an.addr = p;
        uint16_t foff = rom.u16(p + 4);
        bool ok = true;
        size_t cur = foff;
        std::vector<size_t> seen;
        while (cur > 0 && cur < stride && an.frames.size() < 32) {
            bool dup = false; for (size_t s : seen) if (s == cur) { dup = true; break; }
            if (dup) break;
            seen.push_back(cur);
            size_t a2 = p + cur;
            std::vector<std::pair<uint16_t,uint16_t>> subs;
            while (a2 + 2 <= p + stride) {
                uint16_t off = rom.u16(a2);
                if (off & 0x8000) { a2 += 2; break; }       // отрицательный → конец кадра
                if (off >= 4096 || a2 + 4 > p + stride) { ok = false; break; }
                subs.push_back({off, rom.u16(a2 + 2)});
                a2 += 4;
            }
            if (!ok) break;
            an.frames.push_back(std::move(subs));
            if (a2 + 2 > p + stride) break;
            cur = rom.u16(a2);
        }
        if (!ok || an.frames.size() < 2) break;
        // метатекстура анимации = минимальная среди затронутых (для справки)
        uint16_t mn = 0xFFFF;
        for (auto& fr : an.frames) for (auto& s : fr) { uint16_t m = s.first / 16; if (m < mn) mn = m; }
        an.meta = (mn == 0xFFFF) ? 0 : mn;
        out.push_back(std::move(an));
        p += stride;
        if (out.size() > 80) break;
    }
}

void loadLevelFromRom(Level& lv, const Rom& rom, size_t sig, size_t animTableOff) {
    lv.ok = (rom.u32(sig) == 0x5A4D4150u);                 // "ZMAP"
    lv.W = 32; lv.H = 32; lv.floors = Level::MAXF;         // ZT/ZTU: 16 этажей 32×32
    for (int f = 0; f < Level::MAXF; ++f) { lv.fw[f] = 32; lv.fh[f] = 32; lv.foff[f] = (uint32_t)f * 1024; }
    lv.mapT.assign(static_cast<size_t>(Level::MAXF) * 1024, 0);
    for (size_t i = 0; i < lv.mapT.size(); ++i) lv.mapT[i] = rom.u8(sig + 0x1904 + i);
    for (int i = 0; i < 256;      ++i) lv.celltypesT[i] = rom.u8(sig + 0x1804 + i);
    for (int i = 0; i < 256 * 4;  ++i) lv.texorderT[i] = rom.u16(sig + 0x1004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 256 * 8;  ++i) lv.texdefT[i]   = rom.u16(sig + 0x0004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < Level::MAXF; ++i) lv.envT[i] = rom.u8(sig + 0x5904 + i);
    parseWallAnims(rom, sig, animTableOff, lv.wallAnims);
}

// ⭐BZT June: ZMAP эпизода с этажами ПЕРЕМЕННОГО размера [VERIFIED 0xB94A4, findings 2026-07-24].
// hdr = заголовок эпизода (=sig−0x86): [count:w][32×{W:b,H:b,romOff:w от hdr}][2:w]. Дальше "ZMAP",
// texdef @hdr+0x8A, texorder +0x108A, celltypes +0x188A, env 0x10 +0x198A, анимтабл +0x199A.
// Карты этажей лежат по romOff; в mapT складываем подряд (аналог RAM-буфера $FF93A8).
void loadLevelFromRomJune(Level& lv, const Rom& rom, size_t hdr) {
    size_t sig = hdr + 0x86;
    lv.ok = (rom.u32(sig) == 0x5A4D4150u);                 // "ZMAP"
    if (!lv.ok) return;
    int cnt = rom.u16(hdr);
    if (cnt < 1 || cnt > Level::MAXF) { lv.ok = false; return; }
    lv.floors = cnt; lv.W = 0; lv.H = 0;
    lv.mapT.clear();
    uint32_t off = 0;
    for (int f = 0; f < cnt; ++f) {
        int w = rom.u8(hdr + 2 + (size_t)f * 4), h = rom.u8(hdr + 3 + (size_t)f * 4);
        size_t src = hdr + rom.u16(hdr + 4 + (size_t)f * 4);
        lv.fw[f] = (uint16_t)w; lv.fh[f] = (uint16_t)h; lv.foff[f] = off;
        if (w > lv.W) lv.W = w;
        if (h > lv.H) lv.H = h;
        lv.mapT.resize(off + (size_t)w * h);
        for (int i = 0; i < w * h; ++i) lv.mapT[off + i] = rom.u8(src + i);
        off += (uint32_t)(w * h);
    }
    for (int f = cnt; f < Level::MAXF; ++f) { lv.fw[f] = 0; lv.fh[f] = 0; lv.foff[f] = off; }
    for (int i = 0; i < 256;      ++i) lv.celltypesT[i] = rom.u8(sig + 0x1804 + i);
    for (int i = 0; i < 256 * 4;  ++i) lv.texorderT[i] = rom.u16(sig + 0x1004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 256 * 8;  ++i) lv.texdefT[i]   = rom.u16(sig + 0x0004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < Level::MAXF; ++i) lv.envT[i] = (i < 16) ? rom.u8(sig + 0x1904 + i) : 0;
    parseWallAnims(rom, sig, 0x1914, lv.wallAnims);        // BZT: анимтаблица @sig+0x1914
    lv.transitT.clear();                                   // таблица переходов (лифты; лестниц в June нет)
    {
        size_t tt = hdr + rom.u16(hdr + 0x82);
        for (int i = 0; i < 0x40; ++i) {
            size_t o = tt + (size_t)i * 9;
            Level::Transit t{ rom.u8(o), rom.u8(o+1), rom.u8(o+2), rom.u8(o+3), rom.u8(o+4),
                              rom.u8(o+5), rom.u8(o+6), rom.u8(o+7), rom.u8(o+8) };
            if (!t.fl && !t.x && !t.y && !t.dnF && !t.upF) break;   // нулевая запись = конец
            if (t.fl >= cnt) continue;
            lv.transitT.push_back(t);
        }
    }
}
