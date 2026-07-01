#pragma once
// Разрушаемые/секрет-стены ZT (walls_destruct: b130/b168/a6bc). Применение накопленной очереди
// разрушения (cells.hpp::destructQueue) к МУТИРУЕМОМУ Level + таймеры «обломки→пусто».
#include "level.hpp"
#include "cells.hpp"
#include <vector>

// Отложенная очистка обломков (ZT таймер-entry: celltype 0x2d/0x2e → 0 пусто/проход).
struct WallClear { int floor, x, y, t; };
inline std::vector<WallClear>& wallClears() { static std::vector<WallClear> v; return v; }
inline void clearWallState() { destructQueue().clear(); wallClears().clear(); }   // сброс при смене эпизода

// Раз в кадр: применить запросы разрушения к Level + тикнуть таймеры обломков. Возвращает true, если
// что-то РАЗРУШЕНО в этом кадре (для звука разрушения стены — caller играет SFX_WALL).
inline bool applyDestruct(Level& lvl) {
    bool destroyed = false;
    for (const auto& r : destructQueue()) {
        uint8_t ct = lvl.cellType(r.floor, r.x, r.y);
        if (wallIsSecret(ct)) {                                     // 0x79/7B/7D/7F → перманентно celltype+1 (a6bc)
            int rid = lvl.remapCellId((uint8_t)(ct + 1));           //   разрушенная текстура = cell-ID с celltype+1
            if (rid >= 0) { lvl.setCell(r.floor, r.x, r.y, (uint8_t)rid); destroyed = true; }  // смена текстуры
        } else if (wallIsBreakable(ct)) {                           // 0x06/0x83 гориз, 0x07/0x84 верт (b130/b168)
            bool horiz = (ct == 0x06 || ct == 0x83);
            int rid = lvl.remapCellId((uint8_t)(horiz ? 0x2D : 0x2E));  // обломки 0x2D/0x2E (проходимы)
            if (rid >= 0) { lvl.setCell(r.floor, r.x, r.y, (uint8_t)rid);
                            wallClears().push_back({r.floor, r.x, r.y, 45}); }   // таймер → пусто (~0.75с)
            else { int e = lvl.remapCellId(0); lvl.setCell(r.floor, r.x, r.y, (uint8_t)(e < 0 ? 0 : e)); }  // нет обломков → сразу проход
            destroyed = true;
        }
    }
    destructQueue().clear();
    auto& wc = wallClears();                                         // обломки → пустая клетка (проход) по таймеру
    for (size_t i = 0; i < wc.size();) {
        if (--wc[i].t <= 0) {
            int e = lvl.remapCellId(0); lvl.setCell(wc[i].floor, wc[i].x, wc[i].y, (uint8_t)(e < 0 ? 0 : e));
            wc[i] = wc.back(); wc.pop_back();
        } else ++i;
    }
    return destroyed;
}
