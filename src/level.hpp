// ztpp — модель уровня ZMAP (ZT): двойная косвенность клеток.
// Раскладка ZMAP в ROM (подтверждено): +0x4 texdef · +0x1004 texorder · +0x1804 celltypes ·
// +0x1904 map (16 этажей×32×32) · +0x5904 env. Сигнатура "ZMAP".
//
// ЭТАП 0 (мод-готовность): Level ВЛАДЕЕТ данными (массивы), не читает ROM вживую. Наполняется
// loadLevelFromRom() (см. gamedata.hpp) ИЛИ в будущем из .oztd — рендеры/логика об источнике не знают.
#pragma once
#include "rom.hpp"
#include <cstdint>
#include <vector>
#include <utility>

// ── АНИМАЦИЯ СТЕН (по дизасму: аниматор ZT FUN_000023c2, BZT FUN_000022a6) ──
// Таблица анимаций эпизода лежит сразу после env-блока ZMAP (ZT @sig+0x5914, BZT @+0x1914).
// Запись на анимацию: [stride:w][counter:b][period:b][frame_off:w][данные кадров...].
//   Кадр = пары (texdef_off:w, new_tile:w) до отрицательного терминатора, затем смещение след.кадра.
// Каждый кадр ПЕРЕЗАПИСЫВАЕТ слоты texdef (RAM-копия $FF63C6): tile_word_index = texdef_off/2,
//   метатекстура = texdef_off/16. Аниматор по таймеру period циклит кадры (мигание экранов/ламп/глаз).
//   Применение КУМУЛЯТИВНО (как в игре — RAM не сбрасывается между кадрами), пустой кадр = без изменений.
struct WallAnim {
    uint16_t meta   = 0;                                    // метатекстура (для справки/отладки)
    uint8_t  period = 1;                                    // игр.кадров между сменой кадра
    size_t   addr   = 0;                                    // адрес записи в ROM (отладка)
    std::vector<std::vector<std::pair<uint16_t,uint16_t>>> frames;  // [кадр] → [(texdef_off, тайл)]
};

struct Level {
    static constexpr int W = 32, H = 32, FLOORS = 16;

    bool     ok = false;
    std::vector<uint8_t> mapT;          // FLOORS*32*32 cell-ID (по 1 байту)
    uint8_t  celltypesT[256] = {0};     // cell-ID -> celltype
    uint16_t texorderT[256 * 4] = {0};  // [cell*4 + face] -> индекс метатекстуры
    uint16_t texdefT[256 * 8] = {0};    // [meta*8 + sub] -> тайл-номер (БАЗА; аниматор работает с копией)
    uint8_t  envT[FLOORS] = {0};        // режим темноты по этажу
    std::vector<WallAnim> wallAnims;    // анимации стен эпизода (см. parseWallAnims)

    bool valid() const { return ok; }

    // ШАГ 1: cell-ID из грида карты.
    uint8_t cellId(int floor, int x, int y) const {
        return mapT[static_cast<size_t>(floor) * 1024 + static_cast<size_t>(y) * 32 + x];
    }
    // ЗАПИСЬ cell-ID в грид (разрушение стен: ZT b130/b168/a6bc пишут grid[клетка]=remapTable[...]).
    void setCell(int floor, int x, int y, uint8_t id) {
        if (x < 0 || y < 0 || x >= W || y >= H || floor < 0 || floor >= FLOORS) return;
        mapT[static_cast<size_t>(floor) * 1024 + static_cast<size_t>(y) * 32 + x] = id;
    }
    // remapTable (ZT $FF62C6): первый cell-ID с данным celltype, или −1.
    int remapCellId(uint8_t celltype) const {
        for (int i = 0; i < 256; ++i) if (celltypesT[i] == celltype) return i;
        return -1;
    }
    // ШАГ 2: cell-ID -> celltype (код поведения).
    uint8_t cellType(int floor, int x, int y) const { return celltypesT[cellId(floor, x, y)]; }
    // ШАГ 3 (текстуры): cell-ID -> texorder[грань] -> метатекстура; texdef[мета][sub] -> тайл-номер.
    uint16_t texorder(uint8_t cell, int face) const { return texorderT[static_cast<size_t>(cell) * 4 + face]; }
    uint16_t texdef(uint16_t meta, int sub) const { return texdefT[static_cast<size_t>(meta & 0xFF) * 8 + sub]; }
    uint8_t env(int floor) const { return envT[floor]; }
};

// Разобрать таблицу анимаций стен эпизода (порт parse_wall_anims из ztextractor — НЕ эвристика,
// по дизасму аниматора). animTableOff — смещение таблицы от сигнатуры ZMAP (ZT 0x5914, BZT 0x1914).
inline void parseWallAnims(const Rom& rom, size_t sig, size_t animTableOff,
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

// Наполнить Level из ROM по сигнатуре ZMAP (копирует данные во владение Level).
// animTableOff — смещение таблицы анимаций стен от сигнатуры (build-specific; ZT 0x5914).
inline void loadLevelFromRom(Level& lv, const Rom& rom, size_t sig, size_t animTableOff = 0x5914) {
    lv.ok = (rom.u32(sig) == 0x5A4D4150u);                 // "ZMAP"
    lv.mapT.assign(static_cast<size_t>(Level::FLOORS) * 1024, 0);
    for (size_t i = 0; i < lv.mapT.size(); ++i) lv.mapT[i] = rom.u8(sig + 0x1904 + i);
    for (int i = 0; i < 256;      ++i) lv.celltypesT[i] = rom.u8(sig + 0x1804 + i);
    for (int i = 0; i < 256 * 4;  ++i) lv.texorderT[i] = rom.u16(sig + 0x1004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < 256 * 8;  ++i) lv.texdefT[i]   = rom.u16(sig + 0x0004 + static_cast<size_t>(i) * 2);
    for (int i = 0; i < Level::FLOORS; ++i) lv.envT[i] = rom.u8(sig + 0x5904 + i);
    parseWallAnims(rom, sig, animTableOff, lv.wallAnims);
}
