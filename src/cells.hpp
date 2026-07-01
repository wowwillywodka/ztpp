// ztpp — классификация клеток ZT (целл-тип → поведение), портировано из ztextractor
// (mdgfx/gui.py: CELL_DEFS_ZT → MAP_TYPE_TO_ICON → иконка → категория).
//
// ⚠ КЛЮЧЕВОЕ: celltype 0 = Empty (ПОЛ, проходим); celltype 1 = Wall (стена).
// Цепочка: celltype --CELL_DEFS_ZT--> maptype --MAP_TYPE_TO_ICON--> иконка(0..15).
#pragma once
#include <cstdint>

// Иконки (MAP_ICONS): 0 empty,1 wall,2..5 углы,6 hor_door,7 ver_door,8 pl_start,
// 9 enemy,10 weap,11 item,12 spc_wall,13 spc_cell,14 sht_wall,15 decor.
static const uint8_t MAPTYPE_ICON[17] = {
    0, 1, 2, 3, 4, 5, 6, 7, 13, 8, 9, 1, 10, 11, 12, 14, 15
};

// celltype -> maptype (CELL_DEFS_ZT[ct][0]); по умолчанию 1 (wall).
struct CellClassTable {
    uint8_t maptype[256];
    CellClassTable() {
        for (int i = 0; i < 256; ++i) maptype[i] = 1;
        struct OV { uint8_t ct, mt; };
        static const OV ov[] = {
            {0,0}, {2,2},{3,3},{4,4},{5,5}, {6,6},{7,7},
            {8,8},{9,8},{10,8},{11,8},
            {12,14},{13,14},{14,14},{15,14},{16,14},{17,14},
            {18,8},{19,8},{20,8},{21,8},{22,8},{23,8},{24,8},
            {25,13},{26,12},{27,13},{28,13},{29,13},{30,13},
            {31,12},{32,12},{33,13},{34,12},{35,12},{36,12},{37,13},
            {38,8},{39,8},{40,8},
            {41,10},{42,10},{43,10},
            {44,16},{45,16},{46,16},{47,8},{48,14},
            {49,8},{50,8},{51,8},{52,8},{53,16},{54,12},{55,16},
            {56,14},{57,14},{58,14},{59,14},
            {60,8},{61,8},{62,8},{63,8},
            {64,14},{65,14},{66,14},{67,14},
            {68,8},{69,8},{70,8},{71,8},
            {72,14},{73,14},{74,14},{75,14},
            {76,8},{77,8},{78,8},{79,8},{80,8},{81,8},{82,8},{83,8},
            {84,8},{85,8},{86,8},{87,8},{88,8},{89,8},{90,8},{91,8},
            {92,16},{93,16},{94,16},{95,16},{96,16},{97,16},{98,16},{99,16},{100,16},
            {101,10},{102,10},{103,10},{104,10},{105,10},{106,10},{107,10},
            {108,16},{117,16},{118,16},{119,9},{120,16},
            {121,15},{122,11},{123,15},{124,11},{125,11},{126,11},{127,15},
            {129,8},{130,12},{131,16},{132,16},
        };
        for (const auto& p : ov) maptype[p.ct] = p.mt;
    }
};
// ── Build-выбираемая таблица классификации (мульти-билд) ──
// По умолчанию — ZT. loadGameDataFromRom ставит таблицу по билду через setActiveCellTable.
// (Нем. билд меняет врагов на Alien, прото отличаются — их таблицы добавятся позже.)
inline const CellClassTable& ztCellTable() { static CellClassTable t; return t; }
inline const CellClassTable*& activeCellTablePtr() { static const CellClassTable* p = nullptr; return p; }
inline void setActiveCellTable(const CellClassTable* t) { activeCellTablePtr() = t; }
inline const CellClassTable& cellTable() {
    return activeCellTablePtr() ? *activeCellTablePtr() : ztCellTable();
}
// Таблица классификации по индексу билда (Build enum в gamedata.hpp): сейчас все → ZT.
inline const CellClassTable* cellTableForBuild(int /*build*/) { return &ztCellTable(); }

inline int  cellIcon(uint8_t ct)  { return MAPTYPE_ICON[cellTable().maptype[ct]]; }
inline bool iconWall(int ic)      { return ic==1||ic==2||ic==3||ic==4||ic==5||ic==12||ic==14; }
inline bool iconDoor(int ic)      { return ic==6||ic==7; }

// Стена для РЕНДЕРА луча (стены, углы, спец/разрушаемые стены + двери) — луч останавливается.
inline bool cellRenderWall(uint8_t ct) { int ic = cellIcon(ct); return iconWall(ic) || iconDoor(ic); }
// Блокирует ДВИЖЕНИЕ (стены, но НЕ двери — сквозь двери ходим, как в игре они открываются).
inline bool cellBlocks(uint8_t ct)     { return iconWall(cellIcon(ct)); }
inline bool cellIsDoor(uint8_t ct)     { return iconDoor(cellIcon(ct)); }

// ── СОСТОЯНИЕ ДВЕРЕЙ (0=закрыта..1=открыта) — здесь, чтобы видели и actors (LOS/спавн), и raycaster (рендер). ──
#include <unordered_map>
#include <vector>
inline std::unordered_map<int, double>& doorMap() { static std::unordered_map<int, double> m; return m; }
inline int    doorKey(int f, int x, int y) { return ((f * 32 + y) * 32 + x); }
inline double doorOpen(int f, int x, int y) { auto& m = doorMap(); auto it = m.find(doorKey(f, x, y)); return it == m.end() ? 0.0 : it->second; }

// ── РАЗРУШАЕМЫЕ/СЕКРЕТ-СТЕНЫ (ZT walls_destruct: b130/b168 разруш., a6bc секрет/пуле-метка) ──
//   0x06/0x83 гориз → обломки 0x2D → пусто; 0x07/0x84 верт → 0x2E → пусто;
//   0x79/7B/7D/7F секрет/пуле-метка → перманентно celltype+1 (смена текстуры, остаётся стеной).
inline bool wallIsSecret(uint8_t ct)    { return ct == 0x79 || ct == 0x7B || ct == 0x7D || ct == 0x7F; }
inline bool wallIsBreakable(uint8_t ct) { return ct == 0x06 || ct == 0x07 || ct == 0x83 || ct == 0x84; }
inline bool wallIsDestructible(uint8_t ct) { return wallIsSecret(ct) || wallIsBreakable(ct); }
// Очередь разрушения: копим запросы в const-контексте (луч/взрыв), применяем к мутируемому Level в гл.цикле.
struct DestructReq { int floor, x, y; };
inline std::vector<DestructReq>& destructQueue() { static std::vector<DestructReq> q; return q; }
inline void requestDestruct(int floor, int x, int y) { destructQueue().push_back({floor, x, y}); }

// ── GAME-TRUE коллизия игрока (FUN_0000e1b2, таблица @0xe23e) ────────────────
// celltype → класс: 0 пол/проход, 1 полная стена, 2-5 ДИАГОНАЛИ (полуплоскость по
// суб-позиции), 6-7 двери (проходимы при движении). Диагональ пропускает в открытой
// половине → можно пройти между двумя смежными диагоналями.
static const uint8_t COLL_CLASS[256] = {
    0,1,2,3,4,5,6,7,0,0,0,0,1,1,1,1,
    1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    1,0,0,0,0,0,0,0,1,1,1,1,0,0,0,0,
    1,1,1,1,0,0,0,0,1,1,1,1,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,
    1,0,0,1,1,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};
// Блокирует ли точку (fx,fy=суб-позиция 0..1) клетка celltype ct. Полуплоскости — как e1b2:
// кл2 блок xs>=ys; кл3 блок xs+ys>=1; кл4 блок xs<=ys; кл5 блок xs+ys<=1.
inline bool cellBlockedAt(uint8_t ct, double fx, double fy) {
    switch (COLL_CLASS[ct]) {
        case 0: case 6: case 7: return false;        // пол / двери — проход
        case 1: return true;                          // полная стена
        case 2: return fx >= fy;
        case 3: return fx + fy >= 1.0;
        case 4: return fx <= fy;
        case 5: return fx + fy <= 1.0;
        default: return false;
    }
}
// Коллизия для ВРАГОВ: как у игрока, НО двери ТВЁРДЫЕ (в ZT через двери ходит ТОЛЬКО игрок, враги — нет).
inline bool cellBlockedForEnemy(uint8_t ct, double fx, double fy) {
    return cellIsDoor(ct) ? true : cellBlockedAt(ct, fx, fy);
}
