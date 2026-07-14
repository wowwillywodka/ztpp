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
    lv.mapT.assign(static_cast<size_t>(Level::FLOORS) * 1024, 0);
    for (size_t i = 0; i < lv.mapT.size(); ++i) lv.mapT[i] = rom.u8(sig + 0x1904 + i);
    for (int i = 0; i < 256;      ++i) lv.celltypesT[i] = rom.u8(sig + 0x1804 + i);
    for (int i = 0; i < 256 * 4;  ++i) lv.texorderT[i] = rom.u16(sig + 0x1004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 256 * 8;  ++i) lv.texdefT[i]   = rom.u16(sig + 0x0004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < Level::FLOORS; ++i) lv.envT[i] = rom.u8(sig + 0x5904 + i);
    parseWallAnims(rom, sig, animTableOff, lv.wallAnims);
}
