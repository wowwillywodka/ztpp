// АВТОГЕН из mdgfx/mapres/*.bmp — значки карты ZT (16×16 ARGB). Порядок = cellIcon()/MAP_ICONS.
// Данные (MAP_ICON_PX) — в map_icons.cpp (extern); тут только объявления.
#pragma once
#include <cstdint>

inline constexpr int MAP_ICON_W = 16, MAP_ICON_H = 16, MAP_ICON_COUNT = 16;
extern const uint32_t MAP_ICON_PX[16][256];
