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

// ── Публичные top-down рендеры (реализация в mapview.cpp) ──
void renderMap(FB& fb, const Level& lvl, const Palette& wallPal, const WallBank& wall, int floor, int mode, bool grid);
void drawFullMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam);
void drawPauseMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam);
void renderAtlas(FB& fb, const WallBank& wall, const Palette& wallPal);
void drawMinimap(FB& fb, const Level& lvl, const Camera& cam);
void drawMinimapRect(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh, const Level& lvl, const Camera& cam);
void drawGameMap(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                 const Level& lvl, const Camera& cam, const GameData& gd, bool hasScanner);
