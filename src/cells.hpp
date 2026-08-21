// ztpp — классификация клеток ZT (целл-тип → поведение), портировано из ztextractor
// (mdgfx/gui.py: CELL_DEFS_ZT → MAP_TYPE_TO_ICON → иконка → категория).
//
// ⚠ КЛЮЧЕВОЕ: celltype 0 = Empty (ПОЛ, проходим); celltype 1 = Wall (стена).
// Цепочка: celltype --CELL_DEFS_ZT--> maptype --MAP_TYPE_TO_ICON--> иконка(0..15).
#pragma once
#include "tuning.hpp"   // simDt (fps-инвариантные фазы дверей)
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
            // ⭐ТРУПЫ-ЦЕЛЛТАЙПЫ 0x6C-0x74 [VERIFIED 2026-07-28, дескрипторы @0xAB0C +0x19]: FH/Imp/Hydaca/
            // Revenant/Boss1/Dog/FH-SF/Boss3/Boss2. В таблице была дыра 109-116 (дефолт=СТЕНА) — в ZT-картах
            // они не встречались, а ZTU кладёт их в карту (cellID 0xD2-0xDA, напр. D4=труп Hydaca) →
            // рисовался блок-стена поверх трупа (юзер). Проходимы, рендер = corpse-биллборд (decorCorpseSlot).
            {108,16},{109,16},{110,16},{111,16},{112,16},{113,16},{114,16},{115,16},{116,16},
            {117,16},{118,16},{119,9},{120,16},
            // 0x79..0x80 = простреливаемые/секретные стены. 3D-диспетчер ROM
            // ($923a -> $9458) рисует всё семейство стеной; прежняя разметка
            // 0x79/7B/7F как decor пропускала луч сквозь них и брала не тот путь
            // метатекстуры. После попадания типы переходят в 0x7A/7C/7E/80.
            {121,1},{122,1},{123,1},{124,1},{125,1},{126,1},{127,1},{128,1},
            {129,8},{130,12},{131,15},{132,15},  // 0x83(гориз)/0x84(верт) = ФЕЙК-ДВЕРИ (CELL_DEFS «Fake horiz/vert door»): рисуются (icon14 sht_wall, тайл 33)
            //   + БЛОКИРУЮТ (collision) + камера-тревога 0x26 УБИРАЕТ (b130 гориз/b168 верт, requestDestruct→пусто). Пуля/граната НЕ ломают.
            //   ⚠ztextractor CELL_DEFS даёт maptype16→icon15(decor)=НЕВЕРНО (не блокировали); порт → icon14 (game-true). Пример: cellId 0xA7→0x84 (e2f5), 0xA8→0x83 (e2f6).
        };
        for (const auto& p : ov) maptype[p.ct] = p.mt;
    }
};
// ⭐BZT June: таблица по ИГРОВОЙ семантике [VERIFIED LUT коллизии @0xDC30 + загрузчик 0xB94A4,
// findings «BZT June: СТРУКТУРА ДВИЖКА» 2026-07-24]. Отличия от ZT: враги = ct {8,9,A,27,29-2C,65-6B}
// (в ZT 8-B = окружение); старт = 0x77 (как ZT); 0x79/7A/7C/7E/80 = СТЕНЫ (0x7B/7D/7F секретки);
// дефолт НЕИЗВЕСТНОГО ct = 0 (проход) — по LUT всё неперечисленное проходимо (в ZT дефолт = стена).
// Двери 6/7 и лифты/лестницы (0x12-0x17, 0x31-0x34, 0x50-0x5B) — ZT-аналогия, НЕ верифицированы.
struct JuneCellTable : CellClassTable {
    JuneCellTable() {
        for (int i = 0; i < 256; ++i) maptype[i] = 0;                       // дефолт: проходимо (LUT DC30)
        maptype[1] = 1;                                                     // стена
        for (int i = 2; i <= 5; ++i) maptype[i] = (uint8_t)i;               // углы
        maptype[6] = 6; maptype[7] = 7;                                     // двери [ANALOGY]
        for (int i = 0x0C; i <= 0x11; ++i) maptype[i] = 14;                 // лестничные стены
        for (int i = 0x12; i <= 0x17; ++i) maptype[i] = 8;                  // ступени/переходы
        maptype[0x18] = 8;                                                  // пламя (step-on 12cd8)
        for (int i = 0x19; i <= 0x26; ++i) maptype[i] = 13;                 // предметы (веса позже)
        static const uint8_t wp[] = {0x1A,0x1F,0x20,0x22,0x23,0x24,0x25,0x26};  // оружейные идексы BZT
        for (uint8_t w : wp) maptype[w] = 12;
        // ⭐ВРАГИ June [VERIFIED 2026-07-28 рендер-таблица @0x8A20]: RedRobo 0x29, Purple 0x2A,
        // Man {0x09,0x27,0x66,0x68,0x69,0x6A}, RedMan 0x65, GrenMan 0x08. 0x0A/0x2B/0x2C/0x67/0x6B
        // ведут в 8C3E (пусто) — НЕ враги (прежний список был гипотезой и спавнил мусор).
        static const uint8_t en[] = {0x08,0x09,0x27,0x29,0x2A,0x65,0x66,0x68,0x69,0x6A};
        for (uint8_t e : en) maptype[e] = 10;
        maptype[0x2F] = 8;                                                  // блокер
        maptype[0x30] = 14;                                                 // лифт-стены
        for (int i = 0x31; i <= 0x34; ++i) maptype[i] = 8;                  // лифт [ANALOGY]
        for (int i = 0x50; i <= 0x5B; ++i) maptype[i] = 8;                  // лифт-грани [ANALOGY]
        for (int i = 0x38; i <= 0x3B; ++i) maptype[i] = 14;                 // стены (LUT=1)
        for (int i = 0x40; i <= 0x43; ++i) maptype[i] = 14;
        for (int i = 0x48; i <= 0x4B; ++i) maptype[i] = 14;
        maptype[0x77] = 9;                                                  // старт игрока (как ZT)
        maptype[0x79] = 1; maptype[0x7A] = 1; maptype[0x7C] = 1; maptype[0x7E] = 1; maptype[0x80] = 1;
        maptype[0x7B] = 15; maptype[0x7D] = 15; maptype[0x7F] = 15;         // секрет-стены (отстрел)
        maptype[0x83] = 14; maptype[0x84] = 14;                             // разрушаемые
        maptype[0x81] = 8; maptype[0x82] = 8; maptype[0x85] = 8;            // цель/выход
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
inline const CellClassTable& juneCellTable() { static JuneCellTable t; return t; }
// build = (int)Build: 0 ZT, 1 ZTU, 2 BZT_June, 3 BZT_July, 4 ZT_German.
inline const CellClassTable* cellTableForBuild(int build) {
    if (build == 2) return &juneCellTable();
    return &ztCellTable();
}

inline int  cellIcon(uint8_t ct)  { return MAPTYPE_ICON[cellTable().maptype[ct]]; }
inline bool iconWall(int ic)      { return ic==1||ic==2||ic==3||ic==4||ic==5||ic==12||ic==14; }
inline bool iconDoor(int ic)      { return ic==6||ic==7; }

// Стена для РЕНДЕРА луча (стены, углы, спец/разрушаемые стены + двери) — луч останавливается.
inline bool cellRenderWall(uint8_t ct) { int ic = cellIcon(ct); return iconWall(ic) || iconDoor(ic); }
// Блокирует ДВИЖЕНИЕ (стены, но НЕ двери — сквозь двери ходим, как в игре они открываются).
inline bool cellBlocks(uint8_t ct)     { return iconWall(cellIcon(ct)); }
inline bool cellIsDoor(uint8_t ct)     { return iconDoor(cellIcon(ct)); }
// ⭐ФЕЙК-ДВЕРИ 0x83(гориз)/0x84(верт): ROM рисует их ДВЕРНЫМ рендером (render-fn @0x923a = как двери 0x06/0x07), но они БЛОКИРУЮТ
// (не открываются step-on) и убираются лишь камерой-тревогой. cellRendersDoor = рисовать дверным рендером (створки); cellIsDoor
// (открытие/проход) их НЕ включает → doorOpen=0 всегда → рисуются ЗАКРЫТОЙ дверью + блокируют (cellBlocks icon14 = true).
inline bool isFakeDoor(uint8_t ct)     { return ct == 0x83 || ct == 0x84; }
inline bool cellRendersDoor(uint8_t ct){ return cellIsDoor(ct) || isFakeDoor(ct); }
inline bool doorIsHoriz(uint8_t ct)    { return cellIcon(ct) == 6 || ct == 0x83; }   // гориз-панель (обычн.0x06 icon6 / фейк 0x83)

// ── СОСТОЯНИЕ ДВЕРЕЙ (0=закрыта..1=открыта) — здесь, чтобы видели и actors (LOS/спавн), и raycaster (рендер). ──
#include <unordered_map>
#include <unordered_set>
#include <vector>
inline std::unordered_map<int, double>& doorMap() { static std::unordered_map<int, double> m; return m; }
// ⭐УДЕРЖАНИЕ ДВЕРИ АКТЁРОМ [ROM b3ae-b3d4]: b35c держит/открывает дверь, пока в её клетке актёр с бит4
// (живой враг; смерть чистит бит4 andi #$ff2f @184fa/189fa/15662 → ТРУП дверь НЕ держит). Порт: враги
// помечают дверные клетки сюда (openDoorsAtEnemies), rcUpdateDoors читает как «кто-то в клетке».
inline std::unordered_set<int>& doorHoldSet() { static std::unordered_set<int> s; return s; }
// ⭐СТРИД КЛЕТОЧНЫХ КЛЮЧЕЙ (doorKey/pickKey/огни): ZT/ZTU = 32 (формула прежняя, сейвы совместимы);
// June — этажи до 80 клеток → 128 (ставит loadGameDataFromRom; сейвы June в своём профиле).
inline int&   cellKeyStride() { static int s = 32; return s; }
inline int    cellKey(int f, int x, int y) { const int s = cellKeyStride(); return ((f * s + y) * s + x); }
inline int    doorKey(int f, int x, int y) { return cellKey(f, x, y); }
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
// ── ПРОХОДИМОСТЬ ВРАГОВ: своя ROM-таблица @0x14858 (sub_014852), НЕ таблица игрока! ────────────
// 1=блокирует врага, 0=проход. Отличия от коллизии игрока (COLL_CLASS): враги НЕ ходят по ЛЕСТНИЦАМ
// (0x0c-0x18) и ЛИФТАМ (0x2f-0x34, 0x38-0x5b), блокируются ПЛАМЕНЕМ (0x18) и СЕКРЕТ/РАЗРУШАЕМЫМИ
// стенами (0x79-0x7f, 0x80, 0x83, 0x84); диагонали (0x02-0x05) = ПОЛНЫЙ блок (у игрока — полуплоскость).
// Дизасм 0145aa: bsr 14852 → bne=блок (не шагать), beq=проход + кламп суб-позиции [0x20,0xdf] к краю.
static const uint8_t ENEMY_BLOCK[256] = {
    0,1,1,1,1,1,1,1,0,0,0,0,1,1,1,1,
    1,1,1,1,1,1,1,1,1,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
    1,1,1,1,1,0,0,0,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0,
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
inline bool enemyBlocksCell(uint8_t ct) { return ENEMY_BLOCK[ct] != 0; }

// Коллизия для ВРАГОВ (enemy-LUT @0x14858). Двери (0x06/07): враги ОТКРЫВАЮТ их (jsr b1c4/b202 из 5
// think-хендлеров 0x18d2c/191c8/19bd6/1a4b6/1b226, звук 0x67), апдейтер b35c держит открытой пока в
// клетке актёр с bit4. Вариант БЕЗ контекста двери (пробы): закрытая дверь = препятствие.
inline bool cellBlockedForEnemy(uint8_t ct, double fx, double fy) {
    (void)fx; (void)fy;
    return enemyBlocksCell(ct);                 // LUT (двери 06/07 тоже =1 = препятствие)
}
// Вариант ДЛЯ ДВИЖЕНИЯ (знает клетку). canOpen: только 5 дверь-открывающих типов врагов
// (Sgt 0x29/FH-SF 0x69/Boss1 0x67/Boss2 0x6B/Boss3 0x6A, ZT b1c4/b202). Прочие (FH/Imp/Hydaca/Revenant/Dog)
// двери НЕ открывают — закрытая дверь их БЛОКИРУЕТ (проходят лишь уже открытую). corpse/стаггер → canOpen=false.
// ⭐ROM 18c90-18d76 [VERIFIED 2026-07-28]: опенер с дверью в ПОЛУКЛЕТКЕ по ходу (pos+vel·8, lsl #3)
// ПЕРЕМЕЩАЕТСЯ В КЛЕТКУ двери (18d0c/18d42), создаёт запись b1c4/b202 (+0x67 d740) и, стоя в клетке,
// ДЕРЖИТ её открытой (b3ae) — створка анимируется под ним. Порт: опенеру дверь НЕ преграда — он входит
// в клетку движением, а openDoorsAtEnemies создаёт запись + звук ОДИН раз. Прежний вариант толкал фазу
// здесь (+0.20/тик) и конкурировал с закрытием rcUpdateDoors (−0.25/тик, враг не в клетке) — фаза
// дребезжала у нуля → «зацикленный звук двери» (юзер: Sgt/FH-SF упёрся в дверь).
inline bool enemyBlockedAt(uint8_t ct, int f, int cx, int cy, double fx, double fy, bool canOpen = false) {
    (void)fx; (void)fy;
    if (cellIsDoor(ct)) {
        if (canOpen) return false;               // опенер входит в дверную клетку (ROM: телепорт в клетку + запись)
        return doorOpen(f, cx, cy) < 0.4;        // закрытая (<0.4) блокирует НЕ-опенеров; открытую проходят все
    }
    return enemyBlocksCell(ct);                  // enemy-LUT (НЕ полуплоскость игрока)
}
