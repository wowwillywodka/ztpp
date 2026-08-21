// ztpp — src/mapview.hpp: top-down виды карт (интерфейс). Реализация — mapview.cpp.
// isOpen/gameMapMode здесь (inline) — они нужны и mapview.cpp, и рендер-glue в main.cpp.
#pragma once
#include "framebuffer.hpp"
#include "gamedata.hpp"      // Level/GameData/WallBank/Palette
#include "camera.hpp"        // Camera
#include "cells.hpp"         // cellIcon/cellRenderWall/cellBlocks/cellIsDoor/doorOpen
#include "map_icons.hpp"     // MAP_ICON_*
#include "ui.hpp"            // drawText/drawTextBigC/drawTextFontC/uiScale/mapShowIds
#include "map_render.hpp"    // mapres:: (кирпичики пауза-карты)
#include "radar_render.hpp"  // radarres:: (тайлы радара)
#include "actors.hpp"        // actors()/Actor/AT_ENEMY/aliveEnemies (блипы врагов на радаре)
#include <cstdint>

// «Открытая» (НЕ стена) — в режиме textured рисуем текстуру только для стен. Общий хелпер.
inline bool isOpen(uint8_t ct) { return !cellRenderWall(ct); }
// Тумблер карты: true = игровая (10×10 вокруг игрока, как ZT FUN_e816), false = классическая (вся 32×32).
inline bool& gameMapMode() { static bool v = true; return v; }
// BZT June pause-map scroll: аналог RAM $74C4/$74C6, двигает окно 32×32 клетки поверх карты.
// ROM хранит autmap как packed nibbles (2 cells/byte), но порт рендерит напрямую по cellType,
// поэтому горизонтальный скролл двигается на каждое нажатие, а не визуально "через раз".
inline int& bztPauseMapScrollX() { static int v = 0; return v; }
inline int& bztPauseMapScrollY() { static int v = 0; return v; }
inline void resetBztPauseMapScroll() { bztPauseMapScrollX() = 0; bztPauseMapScrollY() = 0; }
inline int bztPauseMapBaseX(const Level& lvl, int floor, double px) {
    if (floor < 0 || floor >= lvl.floors || lvl.fw[floor] <= 32) return 0;
    int v = (int)px - 16;
    if (v < 0) v = 0;
    if (v > (int)lvl.fw[floor] - 32) v = (int)lvl.fw[floor] - 32;
    return v;
}
inline int bztPauseMapBaseY(const Level& lvl, int floor, double py) {
    if (floor < 0 || floor >= lvl.floors || lvl.fh[floor] <= 32) return 0;
    int v = (int)py - 16;
    if (v < 0) v = 0;
    if (v > (int)lvl.fh[floor] - 32) v = (int)lvl.fh[floor] - 32;
    return v;
}
inline bool moveBztPauseMapWindow(const Level& lvl, const Camera& cam, int dx, int dy) {
    int fl = cam.floor;
    if (fl < 0 || fl >= lvl.floors) return false;
    int bx = bztPauseMapBaseX(lvl, fl, cam.px), by = bztPauseMapBaseY(lvl, fl, cam.py);
    int minX = -bx, maxX = ((int)lvl.fw[fl] > 32 ? (int)lvl.fw[fl] - 32 : 0) - bx;
    int minY = -by, maxY = ((int)lvl.fh[fl] > 32 ? (int)lvl.fh[fl] - 32 : 0) - by;
    int nx = bztPauseMapScrollX() + dx, ny = bztPauseMapScrollY() + dy;
    if (nx < minX) nx = minX; if (nx > maxX) nx = maxX;
    if (ny < minY) ny = minY; if (ny > maxY) ny = maxY;
    bool changed = nx != bztPauseMapScrollX() || ny != bztPauseMapScrollY();
    bztPauseMapScrollX() = nx; bztPauseMapScrollY() = ny;
    return changed;
}

// ── Публичные top-down рендеры (реализация в mapview.cpp) ──
void renderMap(FB& fb, const Level& lvl, const Palette& wallPal, const WallBank& wall, int floor, int mode, bool grid);
void drawFullMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam);
void drawPauseMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam, const std::vector<std::string>* lines = nullptr);
void renderAtlas(FB& fb, const WallBank& wall, const Palette& wallPal);
void drawMinimap(FB& fb, const Level& lvl, const Camera& cam);
void drawMinimapRect(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh, const Level& lvl, const Camera& cam);
void drawGameMap(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                 const Level& lvl, const Camera& cam, const GameData& gd, bool hasScanner);
