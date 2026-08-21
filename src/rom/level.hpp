// ztpp — модель уровня ZMAP (ZT): двойная косвенность клеток.
// Раскладка ZMAP в ROM (подтверждено): +0x4 texdef · +0x1004 texorder · +0x1804 celltypes ·
// +0x1904 map (16 этажей×32×32) · +0x5904 env. Сигнатура "ZMAP".
//
// ЭТАП 0 (мод-готовность): Level ВЛАДЕЕТ данными (массивы), не читает ROM вживую. Наполняется
// loadLevelFromRom() ИЛИ в будущем из .oztd — рендеры/логика об источнике не знают.
//
// МОДУЛЬ src/rom: заголовок = структуры + ГОРЯЧИЕ аксессоры (cellId/cellType/texorder — на каждый
// луч, остаются inline). Загрузчики (loadLevelFromRom/parseWallAnims, раз на уровень) — в level.cpp.
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
    // ⭐МУЛЬТИ-БИЛД (2026-07-24): у BZT June этажи ПЕРЕМЕННОГО размера (22×20…80×58, счёт из
    // заголовка ZMAP), у ZT/ZTU — 16 этажей 32×32. W/H = МАКСИМУМ по этажам уровня (границы
    // внешних циклов), фактические размеры этажа — fw[]/fh[], раскладка mapT — foff[].
    static constexpr int MAXF = 16;     // максимум этажей (ZT 16; June ≤10)
    int W = 32, H = 32;                 // макс. размеры этажа уровня (ZT: 32×32)
    int floors = 16;                    // число этажей (ZT 16; June — из заголовка эпизода)

    bool     ok = false;
    std::vector<uint8_t> mapT;          // клетки всех этажей подряд (ZT: 16×1024; June: Σ fw·fh)
    uint16_t fw[MAXF] = {0}, fh[MAXF] = {0};   // размеры этажа
    uint32_t foff[MAXF] = {0};                 // офсет этажа в mapT
    uint8_t  celltypesT[256] = {0};     // cell-ID -> celltype
    uint16_t texorderT[256 * 4] = {0};  // [cell*4 + face] -> индекс метатекстуры
    uint16_t texdefT[256 * 8] = {0};    // [meta*8 + sub] -> тайл-номер (БАЗА; аниматор работает с копией)
    uint8_t  envT[MAXF] = {0};          // режим темноты по этажу
    // ⭐ТАБЛИЦА ПЕРЕХОДОВ June [VERIFIED 0xAE9C/0xAF54]: связность этажей ЯВНАЯ (этажи разного
    // размера — скан колонки как в ZT невозможен). Запись 9 байт: {этаж,x,y | вниз (fl,x,y) |
    // вверх (fl,x,y)}, 0xFF = направления нет. ROM: заголовок эпизода + u16(hdr+0x82), до 0x40 записей.
    struct Transit { uint8_t fl, x, y, dnF, dnX, dnY, upF, upX, upY; };
    std::vector<Transit> transitT;      // пусто = не-June (ZT/ZTU: скан колонки)
    const Transit* findTransit(int floor, int cx, int cy) const {
        for (const Transit& t : transitT)
            if (t.fl == floor && t.x == cx && t.y == cy) return &t;
        return nullptr;
    }
    std::vector<WallAnim> wallAnims;    // анимации стен эпизода (см. parseWallAnims)

    bool valid() const { return ok; }

    // ⭐ОБЛАСТЬ уровня (ROM -$58e4 = (levelByte>>4)&3 = номер эпизода): выбирает набор «небесных»
    // cellID для пуль (13c36/13cbc — окна станции / парапеты крыши). Ставит loadGameDataFromRom.
    int area = 0;

    // ШАГ 1: cell-ID из грида карты. Вне фактических границ этажа (June: этажи < W×H) — 0 (пусто).
    uint8_t cellId(int floor, int x, int y) const {
        if ((unsigned)x >= fw[floor] || (unsigned)y >= fh[floor]) return 0;
        return mapT[foff[floor] + static_cast<size_t>(y) * fw[floor] + x];
    }
    // УНИКАЛЬНЫЙ ключ клетки (замена старого floor*1024+y*32+x: у June этажи разного размера) —
    // линейный индекс в mapT; −1 вне карты. Для наборов doorMap/pickups/потушенного огня.
    int cellIndex(int floor, int x, int y) const {
        if (floor < 0 || floor >= floors || (unsigned)x >= fw[floor] || (unsigned)y >= fh[floor]) return -1;
        return (int)(foff[floor] + static_cast<size_t>(y) * fw[floor] + x);
    }
    // ЗАПИСЬ cell-ID в грид (разрушение стен: ZT b130/b168/a6bc пишут grid[клетка]=remapTable[...]).
    void setCell(int floor, int x, int y, uint8_t id) {
        if (floor < 0 || floor >= floors || (unsigned)x >= fw[floor] || (unsigned)y >= fh[floor]) return;
        mapT[foff[floor] + static_cast<size_t>(y) * fw[floor] + x] = id;
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
void parseWallAnims(const Rom& rom, size_t sig, size_t animTableOff, std::vector<WallAnim>& out);

// Наполнить Level из ROM по сигнатуре ZMAP (копирует данные во владение Level).
// animTableOff — смещение таблицы анимаций стен от сигнатуры (build-specific; ZT 0x5914).
void loadLevelFromRom(Level& lv, const Rom& rom, size_t sig, size_t animTableOff = 0x5914);
// ⭐BZT June: ZMAP с этажами переменного размера; hdr = ЗАГОЛОВОК эпизода (=sig−0x86).
void loadLevelFromRomJune(Level& lv, const Rom& rom, size_t hdr);
