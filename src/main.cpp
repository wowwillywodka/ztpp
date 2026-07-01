// ztpp — Zero Tolerance C++ port, прототип (Фаза 0–2).
//
// Режимы (TAB): 0 celltype-карта · 1 текстурная карта · 2 атлас банка стен ·
//               3 FPS-вид (рейкастер, вид от 1-го лица по реальной геометрии ZT).
//
// Конвейер (валидирован): ROM -> CRAM-палитра -> 32x32 тайлы (column-major) ->
// ZMAP с ДВОЙНОЙ КОСВЕННОСТЬЮ (cell-ID -> celltype / texorder -> texdef -> тайл).
//
// Управление: 1/2/3 эпизод · TAB режим · G сетка · , / . (или [ ]) — этаж ·
//   FPS:    W/S или ↑/↓ — вперёд/назад · A/D — стрейф · ←/→ — поворот · N — noclip
//   ESC/Q — выход.
//
// Без окна: `ztpp <rom> --dump out.ppm [--ep N] [--floor N] [--mode N]`.

#include "rom.hpp"
#include "gfx.hpp"
#include "level.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <algorithm>

// ROM-адреса ZT-релиза вынесены в gamedata.hpp::loadGameDataFromRom (ЭТАП 0: модель данных).

// ----- Софт-framebuffer ----- (FBW/FBH = 640×render_scale; задаются в main до создания FB/окна)
static int FBW = 640, FBH = 640;
// Регион ИГРЫ внутри fb (для аспект-корректного блита в окно — вариант A). Reference = 640×448 (320×224 ×2),
// прочие режимы = весь fb. Презентуем ВЕСЬ fb, но dest-аспект считаем так, чтобы ИГРА была верной пропорции.
static int g_viewX = 0, g_viewY = 0, g_viewW = FBW, g_viewH = FBH;
struct FB {
    std::vector<uint32_t> px;
    FB() : px(static_cast<size_t>(FBW) * FBH, 0xFF000000u) {}
    void clear(uint32_t c = 0xFF101014u) { std::fill(px.begin(), px.end(), c); }
    inline void put(int x, int y, uint32_t c) {
        if (x >= 0 && x < FBW && y >= 0 && y < FBH) px[static_cast<size_t>(y) * FBW + x] = c;
    }
    void rect(int x0, int y0, int w, int h, uint32_t c) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) put(x, y, c);
    }
};

#include "raycaster.hpp"
#include "faithful.hpp"
#include "ui.hpp"          // экранное меню настроек (ESC) + шрифт + сохранение в файл
#include "map_icons.hpp"   // значки клеток карты (16×16, из mdgfx/mapres) — рисуются по типу клетки
#include "walls.hpp"       // разрушаемые/секрет-стены (применение очереди разрушения к Level)
#include "weapons.hpp"     // инвентарь / подбор / выбор оружия / отрисовка в руках (после FB + ui)
#include "messages.hpp"    // HUD-сообщения ZT (очередь, нижний-левый угол) — после ui + actors
#include "console.hpp"     // внутриигровая консоль (`): команды give/spawn/god/…, история, автодополнение, логи
#include "sound.hpp"       // PCM-звук эффектов (SDL audio): загрузка сэмплов + микшер + триггеры событий

// Цвет клетки по ИКОНКЕ (как карта в ztextractor). Режим celltype.
static uint32_t celltypeColor(uint8_t ct) {
    switch (cellIcon(ct)) {
        case 0:  return 0xFF222A33u;                                   // empty floor
        case 1: case 2: case 3: case 4: case 5: return 0xFFA8A8B0u;    // wall/corners
        case 6: case 7:  return 0xFF6A8CD0u;                           // door
        case 8:  return 0xFF2868FFu;                                   // player start
        case 9:  return 0xFFD83434u;                                   // enemy
        case 10: return 0xFFF0C020u;                                   // weapon
        case 11: return 0xFF32C832u;                                   // item
        case 12: return 0xFF8B5A2Bu;                                   // spc_wall
        case 13: return 0xFF2E4A6Eu;                                   // spc_cell/trigger
        case 14: return 0xFFB044C4u;                                   // sht_wall (destructible)
        case 15: return 0xFF50C0A0u;                                   // decor
        default: return 0xFFA8A8B0u;
    }
}
// «Открытая» (НЕ стена) — в режиме textured рисуем текстуру только для стен.
static bool isOpen(uint8_t ct) { return !cellRenderWall(ct); }

static void blitTile(FB& fb, const uint8_t idx[32 * 32], const Palette& pal,
                     int dx, int dy, int cellpx, bool transparent0) {
    for (int yy = 0; yy < cellpx; ++yy) {
        int sy = yy * 32 / cellpx;
        for (int xx = 0; xx < cellpx; ++xx) {
            int sx = xx * 32 / cellpx;
            uint8_t i = idx[sy * 32 + sx];
            if (transparent0 && i == 0) continue;
            fb.put(dx + xx, dy + yy, pal.c[i]);
        }
    }
}

// ===== Режимы карты (top-down) =====
// Значок клетки (16×16 из mapres) масштабированный в size×size, nearest.
static void blitMapIcon(FB& fb, int ic, int dx, int dy, int size) {
    if (ic < 0 || ic >= MAP_ICON_COUNT) ic = 1;             // дефолт — стена
    const uint32_t* px = MAP_ICON_PX[ic];
    for (int yy = 0; yy < size; ++yy)
        for (int xx = 0; xx < size; ++xx)
            fb.put(dx + xx, dy + yy, px[(yy * MAP_ICON_H / size) * MAP_ICON_W + (xx * MAP_ICON_W / size)]);
}
// 2-значный hex cell-ID поверх значка (белый с чёрной подложкой для читаемости).
static void drawCellId(FB& fb, int dx, int dy, int size, uint8_t cid) {
    char s[4]; std::snprintf(s, sizeof(s), "%02X", cid);
    int tx = dx + (size - 16) / 2, ty = dy + (size - 8) / 2;  // центр (8x8 шрифт ×... size 1 = 16px на 2 символа)
    drawText(fb, tx + 1, ty + 1, s, 0xFF000000u, 1);          // тень
    drawText(fb, tx,     ty,     s, 0xFFFFFFFFu, 1);          // текст
}
static void renderMap(FB& fb, const Level& lvl, const Palette& wallPal, const WallBank& wall,
                      int floor, int mode, bool grid) {
    fb.clear(0xFF0A0A0Eu);
    const int cellpx = FBW / Level::W; // 20
    uint8_t tile[32 * 32];
    for (int y = 0; y < Level::H; ++y) {
        for (int x = 0; x < Level::W; ++x) {
            int dx = x * cellpx, dy = y * cellpx;
            uint8_t cell = lvl.cellId(floor, x, y);
            uint8_t ct   = lvl.cellType(floor, x, y);
            if (mode == 1 && !isOpen(ct)) {                  // textured: реальные тайлы стен
                uint16_t meta    = lvl.texorder(cell, 0);
                uint16_t tileNum = lvl.texdef(meta, 0);
                if (tileNum < (uint16_t)wall.count) {
                    wall.decode(tileNum, tile);
                    blitTile(fb, tile, wallPal, dx, dy, cellpx, false);
                } else {
                    fb.rect(dx, dy, cellpx, cellpx, 0xFFA8A8B0u);
                }
            } else {                                         // celltype: ЗНАЧОК по типу клетки (как ztextractor «Карты»)
                blitMapIcon(fb, cellIcon(ct), dx, dy, cellpx);
            }
            if (grid) {
                for (int i = 0; i < cellpx; ++i) {
                    fb.put(dx + i, dy, 0xFF000000u);
                    fb.put(dx, dy + i, 0xFF000000u);
                }
            }
            if (mapShowIds()) drawCellId(fb, dx, dy, cellpx, cell);   // cell ID поверх (тумблер в настройках)
        }
    }
}

// ПАУЗА-КАРТА (по TAB, как меню паузы ZT — НЕ радар): полная карта уровня (значки celltype) + позиция/направление
// игрока + «MAP» сверху. Верхушку ZT (уровень/NOT SECURED/пароль) пока не делаем — просто заголовок MAP.
// ── ПЛОСКАЯ ПОЛНАЯ КАРТА (меню настроек «full map»): только автокарта этажа + позиция, без хедера ──
static void drawFullMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam) {
    renderMap(fb, gd.levels[ep], gd.wallPal, gd.wall, floor, 0, true);
    const int cellpx = FBW / Level::W;
    int px = (int)(cam.px * cellpx), py = (int)(cam.py * cellpx);
    for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx)
        if (dx*dx + dy*dy <= 10) fb.put(px + dx, py + dy, 0xFF40FF40u);
    for (int i = 0; i < 12; ++i) fb.put(px + (int)(cam.dirX*i), py + (int)(cam.dirY*i), 0xFFFFFF00u);
}

// ── МЕНЮ ПАУЗЫ (Tab) — как в оригинале: автокарта этажа + ХЕДЕР (здание/этаж · SECURED/NOT SECURED · пароль).
// Карта = renderMap (значки/двери/предметы). Позиция игрока = мигающая зелёная точка + жёлтая стрелка.
// SECURED = на этаже не осталось врагов (aliveEnemies==0). ⚠ Точный 8-симв. ПАРОЛЬ (ZT gen 0x58720:
// упаковка прогресса + checksum + feedback-XOR 0x586b4 + таблица 0x585ea) — отдельный проход.
static void drawPauseMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam) {
    // МЕНЮ ПАУЗЫ как в ОРИГИНАЛЕ: фон = руки держат карту-PDA (ROM @0x12DD06, gd.pauseBg 320×224),
    // на поверхность карты рисуется blueprint этажа (комнаты светлые, стены-контур тёмные, двери оранж.) +
    // позиция игрока + сверху «<имя уровня> : *SECURED*». Без пароля.
    const Level& lvl = gd.levels[ep];
    int us = uiScale();
    fb.clear(0xFF000000u);
    if (gd.pauseBg.empty()) {                                  // фон не загрузился → тёмный фон
        fb.clear(0xFF04040Au);
    } else {
        double sc = (double)FBW/320.0; { double sy=(double)FBH/224.0; if (sy<sc) sc=sy; }   // вписать 320×224
        int vpW=(int)(320*sc), vpH=(int)(224*sc), vpX=(FBW-vpW)/2, vpY=(FBH-vpH)/2;
        for (int y=0;y<vpH;++y){ int ny=(int)(y/sc); if(ny>223)ny=223;
            const uint32_t* src=&gd.pauseBg[(size_t)ny*320];
            for(int x=0;x<vpW;++x){ int nx=(int)(x/sc); if(nx>319)nx=319; fb.put(vpX+x, vpY+y, src[nx]); } }
        // BLUEPRINT этажа: КОМНАТЫ светлые + ТОНКИЕ тёмные стены-края + двери оранж. Чисто (без перекрытий).
        const int nx0=96, ny0=76, cellN=4; int cw=(int)(cellN*sc); if(cw<2)cw=2; int ew=(int)sc; if(ew<1)ew=1;
        auto wallAt=[&](int x,int y){ return x<0||y<0||x>=Level::W||y>=Level::H || cellBlocks(lvl.cellType(floor,x,y)); };
        auto fbx=[&](int cx){ return vpX+(int)((nx0+cx*cellN)*sc); }; auto fby=[&](int cy){ return vpY+(int)((ny0+cy*cellN)*sc); };
        for (int cy=0;cy<Level::H;++cy) for (int cx=0;cx<Level::W;++cx) {  // пол открытых клеток
            uint8_t ct=lvl.cellType(floor,cx,cy); if (cellBlocks(ct)) continue;
            int ic=cellIcon(ct);
            uint32_t fl = cellIsDoor(ct) ? 0xFFE08828u : (ic==10||ic==11) ? 0xFFE0C840u : 0xFFC0B8DCu;
            fb.rect(fbx(cx),fby(cy),cw,cw,fl);
        }
        for (int cy=0;cy<Level::H;++cy) for (int cx=0;cx<Level::W;++cx) {  // тонкие стены-края у границ комнат
            if (wallAt(cx,cy)) continue; int fx=fbx(cx), fy=fby(cy); const uint32_t W=0xFF4C4C54u;
            if (wallAt(cx,cy-1)) fb.rect(fx,fy,cw,ew,W);
            if (wallAt(cx,cy+1)) fb.rect(fx,fy+cw-ew,cw,ew,W);
            if (wallAt(cx-1,cy)) fb.rect(fx,fy,ew,cw,W);
            if (wallAt(cx+1,cy)) fb.rect(fx+cw-ew,fy,ew,cw,W);
        }
        // позиция игрока: мигающая точка
        int pfx=vpX+(int)((nx0+cam.px*cellN)*sc), pfy=vpY+(int)((ny0+cam.py*cellN)*sc);
        static int blink=0; ++blink;
        if((blink>>3)&1) for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx) if(dx*dx+dy*dy<=5) fb.put(pfx+dx,pfy+dy,0xFF40FF40u);
        // ВЕРХНИЙ ТЕКСТ: имя уровня + статус (как ориг. «DOCKING BAY LEVEL 1 : *SECURED*»)
        int gi=ep*16+floor; if(gi<0)gi=0; if(!gd.levelNames.empty()&&gi>=(int)gd.levelNames.size())gi=(int)gd.levelNames.size()-1;
        std::string nm = (!gd.levelNames.empty()&&gi>=0)?gd.levelNames[gi]:"";
        bool secured=(aliveEnemies(floor)==0);
        char buf[80]; std::snprintf(buf,sizeof buf,"%s : %s", nm.c_str(), secured?"*SECURED*":"*NOT SECURED*");
        drawTextBigC(fb, FBW/2, vpY+6*us, buf, 0xFFFFFFFFu, 2*us);             // ВЕРХ: большой шрифт (Font_grph)
        // НИЗ: имя уровня шрифтом Font2 (gd.fontAlt) под картой — как в оригинале
        if (!nm.empty()) drawTextFontC(fb, FBW/2, vpY+(int)(206*sc), nm.c_str(), 0xFF303038u,
                                       sc > 1.5 ? 2 : 1, gd.fontAlt.have ? &gd.fontAlt : nullptr);
    }
}

static void renderAtlas(FB& fb, const WallBank& wall, const Palette& wallPal) {
    fb.clear(0xFF101018u);
    const int cols = FBW / 32; // 20
    uint8_t tile[32 * 32];
    for (int t = 0; t < wall.count; ++t) {
        int cx = (t % cols) * 32, cy = (t / cols) * 32;
        if (cy + 32 > FBH) break;
        wall.decode(t, tile);
        blitTile(fb, tile, wallPal, cx, cy, 32, false);
    }
}

// ===== Миникарта поверх FPS-вида =====
static void drawMinimap(FB& fb, const Level& lvl, const Camera& cam) {
    const int s = 5, ox = 8, oy = 8;
    fb.rect(ox - 2, oy - 2, Level::W * s + 4, Level::H * s + 4, 0xFF000000u);
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x) {
            uint8_t ct = lvl.cellType(cam.floor, x, y);
            uint32_t c = cellBlocks(ct) ? 0xFF707080u
                       : (cellIsDoor(ct) ? 0xFF6A8CD0u : 0xFF222A33u);
            fb.rect(ox + x * s, oy + y * s, s - 1, s - 1, c);
        }
    int cxp = ox + (int)(cam.px * s), cyp = oy + (int)(cam.py * s);
    for (int i = 0; i < 12; ++i)
        fb.put(cxp + (int)(cam.dirX * i), cyp + (int)(cam.dirY * i), 0xFFFFFF00u);
    fb.rect(cxp - 1, cyp - 1, 3, 3, 0xFF20FF20u);
}

// Та же миникарта, но в ПРОИЗВОЛЬНЫЙ буфер и прямоугольник (для радар-области HUD в референс-режиме).
// Карта (32×32 клетки) вписывается с центрированием; масштаб = max целое, влезающее в прямоугольник.
static void drawMinimapRect(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                            const Level& lvl, const Camera& cam) {
    auto put = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < bw && y >= 0 && y < bh) buf[(size_t)y * bw + x] = c;
    };
    int s = std::min(rw / Level::W, rh / Level::H); if (s < 1) s = 1;
    int mw = Level::W * s, mh = Level::H * s;
    int ox = rx + (rw - mw) / 2, oy = ry + (rh - mh) / 2;
    for (int y = ry; y < ry + rh; ++y)                       // тёмная подложка на всю область радара
        for (int x = rx; x < rx + rw; ++x) put(x, y, 0xFF000000u);
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x) {
            uint8_t ct = lvl.cellType(cam.floor, x, y);
            uint32_t c = cellBlocks(ct) ? 0xFF707080u
                       : (cellIsDoor(ct) ? 0xFF6A8CD0u : 0xFF222A33u);
            for (int dy = 0; dy < s; ++dy)
                for (int dx = 0; dx < s; ++dx) put(ox + x * s + dx, oy + y * s + dy, c);
        }
    int cxp = ox + (int)(cam.px * s), cyp = oy + (int)(cam.py * s);
    for (int i = 0; i < 8; ++i)                              // вектор направления
        put(cxp + (int)(cam.dirX * i), cyp + (int)(cam.dirY * i), 0xFFFFFF00u);
    for (int dy = -1; dy <= 1; ++dy)                         // маркер игрока
        for (int dx = -1; dx <= 1; ++dx) put(cxp + dx, cyp + dy, 0xFF20FF20u);
}

// (Враги: размер drawW/drawH, анимация ходьбы g_enemyWalk, hit-отлёт+труп, нокдаун игрока — actors.hpp/raycaster.hpp.)
// Тумблер карты: true = игровая (10×10 вокруг игрока, как ZT FUN_e816), false = классическая (вся 32×32).
inline bool& gameMapMode() { static bool v = true; return v; }

// ── Тонкий 3×5 шрифт для hex на маленьком радаре (16 глифов 0-F, по 5 строк × 3 бита) ──
static const uint8_t TINY_HEX[16][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,2,2,2},
    {7,5,7,5,7},{7,5,7,1,7},{2,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,6,4,7},{7,4,6,4,4}
};
static inline void putBuf(uint32_t* b, int bw, int bh, int x, int y, uint32_t c) {
    if (x >= 0 && x < bw && y >= 0 && y < bh) b[(size_t)y * bw + x] = c;
}
static void drawTinyDigit(uint32_t* b, int bw, int bh, int x, int y, int d, uint32_t col) {
    if (d < 0 || d > 15) return;
    for (int r = 0; r < 5; ++r) for (int c = 0; c < 3; ++c)
        if (TINY_HEX[d][r] & (4 >> c)) putBuf(b, bw, bh, x + c, y + r, col);
}
// 2-значный hex (0..255) тонким шрифтом 3×5, с чёрной тенью (+1,+1) для читаемости поверх значка.
static void drawTinyHexByte(uint32_t* b, int bw, int bh, int x, int y, uint8_t v, uint32_t col) {
    drawTinyDigit(b, bw, bh, x + 1, y + 1, v >> 4,  0xFF000000u);
    drawTinyDigit(b, bw, bh, x + 5, y + 1, v & 0xF, 0xFF000000u);
    drawTinyDigit(b, bw, bh, x,     y,     v >> 4,  col);
    drawTinyDigit(b, bw, bh, x + 4, y,     v & 0xF, col);
}
// Значок карты (16×16) масштаб в size×size в произвольный буфер (для радара).
static void blitIconBuf(uint32_t* b, int bw, int bh, int ic, int dx, int dy, int size) {
    if (ic < 0 || ic >= MAP_ICON_COUNT) ic = 1;
    const uint32_t* px = MAP_ICON_PX[ic];
    for (int yy = 0; yy < size; ++yy) for (int xx = 0; xx < size; ++xx)
        putBuf(b, bw, bh, dx + xx, dy + yy, px[(yy * MAP_ICON_H / size) * MAP_ICON_W + (xx * MAP_ICON_W / size)]);
}

// ИГРОВАЯ КАРТА (как ZT FUN_0000e816): окно 10×10 клеток ВОКРУГ игрока (скролл, центр = игрок), тайл-типы
// пол/стена/угол/дверь (LUT @0xE23E → 8 chars), цвета палитры карты (линия 3 = 0x20D2: стена idx7 жёлт,
// пол чёрн, враг idx5 красн). Игрок — стрелка по направлению в центре; враги — красные блипы (живые актёры).
static void drawGameMap(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                        const Level& lvl, const Camera& cam, const GameData& gd) {
    auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && x < bw && y >= 0 && y < bh) buf[(size_t)y * bw + x] = c; };
    const int N = 10, s = rw / N;                                    // 10×10 окно, 8px/клетка (rw=80)
    uint32_t cWall = gd.heldPal.c[7], cFloor = 0xFF080C08u, cGrid = 0xFF1C2A1Cu, cDoor = gd.heldPal.c[6];
    for (int y = ry; y < ry + rh; ++y) for (int x = rx; x < rx + rw; ++x) put(x, y, 0xFF000000u);  // подложка
    int pcx = (int)cam.px, pcy = (int)cam.py;                        // клетка игрока
    int ox = pcx - N / 2, oy = pcy - N / 2;                          // окно центрировано на игроке…
    if (ox < 0) ox = 0; if (ox > Level::W - N) ox = Level::W - N;    // …с кламом к границам карты (как ZT 5..27)
    if (oy < 0) oy = 0; if (oy > Level::H - N) oy = Level::H - N;
    for (int gy = 0; gy < N; ++gy)
        for (int gx = 0; gx < N; ++gx) {
            int mx = ox + gx, my = oy + gy;
            if (mx < 0 || my < 0 || mx >= Level::W || my >= Level::H) continue;
            uint8_t ct = lvl.cellType(cam.floor, mx, my);
            int px0 = rx + gx * s, py0 = ry + gy * s;
            if (mapShowIds()) {                                      // ДЕВ-РЕЖИМ: значок клетки + cell ID тонким шрифтом
                blitIconBuf(buf, bw, bh, cellIcon(ct), px0, py0, s);
                drawTinyHexByte(buf, bw, bh, px0 + (s - 7) / 2, py0 + (s - 5) / 2, lvl.cellId(cam.floor, mx, my), 0xFFFFFFFFu);
                continue;
            }
            if (ct >= 2 && ct <= 5) {                                // УГОЛ: жёлтый треугольник (как renderMap)
                bool back = (ct == 2 || ct == 4), ur = (ct == 2), lr = (ct == 3), ll = (ct == 4);
                for (int yy = 0; yy < s; ++yy) for (int xx = 0; xx < s; ++xx) {
                    bool solid = back ? (ur ? (yy <= xx) : (yy >= xx)) : (lr ? (xx + yy >= s) : (xx + yy <= s));
                    put(px0 + xx, py0 + yy, solid ? cWall : cFloor);
                }
            } else if (cellIsDoor(ct)) {                             // ДВЕРЬ: жёлтая со щелью (гор/верт)
                for (int yy = 0; yy < s; ++yy) for (int xx = 0; xx < s; ++xx) put(px0 + xx, py0 + yy, cDoor);
                if (ct == 6) for (int xx = 0; xx < s; ++xx) { put(px0 + xx, py0 + s/2, cFloor); put(px0 + xx, py0 + s/2 - 1, cFloor); }
                else         for (int yy = 0; yy < s; ++yy) { put(px0 + s/2, py0 + yy, cFloor); put(px0 + s/2 - 1, py0 + yy, cFloor); }
            } else if (cellBlocks(ct)) {                             // СТЕНА: сплошной жёлтый
                for (int yy = 0; yy < s; ++yy) for (int xx = 0; xx < s; ++xx) put(px0 + xx, py0 + yy, cWall);
            } else {                                                 // ПОЛ: тёмный + тонкая сетка
                for (int yy = 0; yy < s; ++yy) for (int xx = 0; xx < s; ++xx)
                    put(px0 + xx, py0 + yy, (xx == 0 || yy == 0) ? cGrid : cFloor);
            }
        }
    // ВРАГИ — красные блипы (живые актёры на этаже в окне) — как ZT eb5a/сканер
    for (const Actor& a : actors()) {
        if (!a.active || a.think != AT_ENEMY || a.floor != cam.floor) continue;
        double gxp = (a.x - ox), gyp = (a.y - oy);
        if (gxp < 0 || gyp < 0 || gxp >= N || gyp >= N) continue;
        int ex = rx + (int)(gxp * s), ey = ry + (int)(gyp * s);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) put(ex + dx, ey + dy, gd.heldPal.c[5]);  // красный
    }
    // ИГРОК — зелёный маркер + стрелка направления в центре окна (по суб-позиции)
    int cxp = rx + (int)((cam.px - ox) * s), cyp = ry + (int)((cam.py - oy) * s);
    for (int i = 0; i < 7; ++i) put(cxp + (int)(cam.dirX * i), cyp + (int)(cam.dirY * i), 0xFFFFFFFFu);  // стрелка (белая)
    for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) put(cxp + dx, cyp + dy, 0xFF20FF20u);  // игрок (зелёный)
}

// (см. README: враги — AI/LOS/типы атак в actors.hpp; карта — drawGameMap выше; настройки — ui.hpp)
// HUD оружия: имя текущего ствола + боезапас (низ-лево) + HP. Кулаки = "FISTS" без счётчика.
static void drawWeaponHud(FB& fb, const Inventory& inv) {
    char line[64];
    if (inv.current < 0) std::snprintf(line, sizeof(line), "FISTS");
    else                 std::snprintf(line, sizeof(line), "%s  %d", ITEMS[inv.current].name, inv.ammo[inv.current]);
    drawText(fb, 12, FBH - 28, line, 0xFFFFD050u, 2);   // жёлтый, как HUD оружия ZT
    drawText(fb, 12, FBH - 50, "[Z/X] WEAPON", 0xFF80C0FFu, 1);
    const PlayerState& p = player();                     // HP (низ-право): число + цвет по HP (жёлт/оранж/красн, как ZT)
    char hp[16]; std::snprintf(hp, sizeof(hp), "HP %d", p.hp);
    drawText(fb, FBW - 90, FBH - 28, hp, hpColor(p.hp), 2);
}

static void renderFPStoFB(FB& fb, const GameData& gd, const Level& lvl, const Palette& wallPal,
                          Camera& cam, MetaCache& meta, std::vector<double>& zbuf, bool faithful,
                          const Inventory& inv) {
    int envMode = lvl.env(cam.floor);
    auto putfn = [&](int x, int y, uint32_t c) { fb.put(x, y, c); };
    if (faithful) renderFaithful(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    else          renderFPS(putfn, FBW, FBH, lvl, wallPal, meta, cam, zbuf, envMode);
    drawMinimap(fb, lvl, cam);
    drawHeldWeapon(fb, FBW, FBH, gd, inv);   // оружие в руках (низ-центр)
    drawLaserSight([&](int x, int y, uint32_t c) { fb.put(x, y, c); }, 0, 0, FBW, FBH, inv,
                   zbuf.empty() ? 10.0 : zbuf[FBW / 2]);   // лазерный прицел (id 10): точка по дистанции центр.колонны
    drawWeaponHud(fb, inv);                   // имя + боезапас + HP
    applyDamageFlash(fb.px.data(), FBW * FBH); // вспышка урона (тинт всего кадра)
}

// РЕФЕРЕНС-РЕЖИМ: рендер игры точно как в оригинале — 3D-вид в нативном окошке 128×80 (×2 по гориз.
// = 256×80) внутри кокпит-HUD 320×224, всё масштабируется равномерно в окно. Для точного сравнения
// с игрой и заполнения пространства HUD-ом. Раскладка/HUD — из дизасма (gamedata.hpp).
static void renderReference(FB& fb, const GameData& gd, const Level& lvl, const Palette& wallPal,
                            Camera& cam, MetaCache& meta, std::vector<double>& zbuf, const Inventory& inv) {
    // 1) 3D faithful в ИСТИННОМ нативном ZT 256×80 (focalH=128/focalV=64, неквадратный пиксель 2:1):
    //    128 геом-колонн × 2px, дизер пол/потолок 256 (2 нибла = 2 разных px), стены 256, высота 1:1.
    //    hstretch=1 — растяжки нет (×2 по гориз. уже встроена в нативные 256). Это и есть кадр игры 1:1.
    static std::vector<uint32_t> view; view.assign((size_t)HUD_VW * HUD_VH, 0xFF000000u);
    double savedHs = faHStretch(); faHStretch() = 1.0;
    int envMode = lvl.env(cam.floor);
    renderFaithful([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_VW && y >= 0 && y < HUD_VH) view[(size_t)y * HUD_VW + x] = c; },
                   HUD_VW, HUD_VH, lvl, wallPal, meta, cam, zbuf, envMode, HUD_VW, HUD_VH);
    faHStretch() = savedHs;

    // 2) кадр 320×224 = HUD-кокпит + 3D-вид (256×80) в окно (HUD_VX,HUD_VY) — НАПРЯМУЮ 1:1
    static std::vector<uint32_t> frame; frame.assign((size_t)HUD_W * HUD_H, 0xFF101014u);
    if ((int)gd.hud.size() == HUD_W * HUD_H) std::copy(gd.hud.begin(), gd.hud.end(), frame.begin());
    for (int y = 0; y < HUD_VH; ++y)
        for (int x = 0; x < HUD_VW; ++x)
            frame[(size_t)(HUD_VY + y) * HUD_W + (HUD_VX + x)] = view[(size_t)y * HUD_VW + x];

    // (радар рисуем НИЖЕ — прямо на fb в полном разрешении, чтобы значки/cell ID были мельче и чётче,
    //  а не удваивались вместе с кокпитом ×scale)

    // 2c) ОРУЖИЕ В РУКАХ — ПИКСЕЛЬ-В-ПИКСЕЛЬ как ZT: VDP-спрайт в нативных экранных координатах 320×224
    //     (точные SAT-позиции из дизасма), клип к 3D-окну (кокпит режет низ оружия, как в оригинале).
    drawHeldNative([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, gd, inv);
    drawLaserSight([&](int x, int y, uint32_t c) { if (x >= 0 && x < HUD_W && y >= 0 && y < HUD_H) frame[(size_t)y * HUD_W + x] = c; },
                   HUD_VX, HUD_VY, HUD_VW, HUD_VH, inv, zbuf.empty() ? 10.0 : zbuf[HUD_VW / 2]);  // лазерный прицел id 10

    // 2d) HUD ИНВЕНТАРЯ — карусель иконок оружия (5 верхних панелей) + счётчик боезапаса, текущий по центру.
    drawInventoryHud(frame.data(), HUD_W, HUD_H, gd, inv);
    // 2e) HP — ЧИСЛО справа в кокпите (цвет по здоровью).
    drawHpHud(frame.data(), HUD_W, HUD_H, gd);
    // 2f) ВСПЫШКА УРОНА — тинт всего кадра (палитра краснеет/белеет, как ZT d98e), сила ~ урон, затухает.
    applyDamageFlash(frame.data(), HUD_W * HUD_H);

    // 3) масштаб 320×224 → fb (равномерно, целочисленно, по центру; рамка-леттербокс чёрная)
    fb.clear(0xFF000000u);
    int scale = std::min(FBW / HUD_W, FBH / HUD_H); if (scale < 1) scale = 1;
    int ox = (FBW - HUD_W * scale) / 2, oy = (FBH - HUD_H * scale) / 2;
    for (int y = 0; y < HUD_H; ++y)
        for (int x = 0; x < HUD_W; ++x) {
            uint32_t c = frame[(size_t)y * HUD_W + x];
            for (int sy = 0; sy < scale; ++sy)
                for (int sx = 0; sx < scale; ++sx)
                    fb.put(ox + x * scale + sx, oy + y * scale + sy, c);
        }
    // 4) РАДАР — НА fb в ПОЛНОМ разрешении (значки + cell ID мельче/чётче: шрифт не удваивается с кокпитом).
    //    Позиция = радар-область кокпита (HUD_RX/RY) × scale.
    int rrx = ox + HUD_RX * scale, rry = oy + HUD_RY * scale, rrw = HUD_RW * scale, rrh = HUD_RH * scale;
    if (gameMapMode()) drawGameMap(fb.px.data(), FBW, FBH, rrx, rry, rrw, rrh, lvl, cam, gd);
    else               drawMinimapRect(fb.px.data(), FBW, FBH, rrx, rry, rrw, rrh, lvl, cam);
}

static void render(FB& fb, const GameData& gd,
                   int ep, int floor, int mode, bool grid,
                   Camera& cam, MetaCache& meta, std::vector<double>& zbuf, bool faithful, bool reference,
                   const Inventory& inv) {
    // регион игры в fb для презентации (reference кладёт 320×224 ×2 = 640×448 по центру; иначе весь fb).
    // Презентуем именно его (без запечённых чёрных полос) → 4:3 заполняет окно по высоте.
    if (mode == 3 && reference) {
        g_viewW = HUD_W * 2; g_viewH = HUD_H * 2;
        g_viewX = (FBW - g_viewW) / 2; g_viewY = (FBH - g_viewH) / 2;     // = (0, 96) при scale 2
    } else { g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH; }
    if      (mode == 3 && reference) renderReference(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, inv);
    else if (mode == 3) renderFPStoFB(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, faithful, inv);
    else if (mode == 2) renderAtlas(fb, gd.wall, gd.wallPal);
    else                renderMap(fb, gd.levels[ep], gd.wallPal, gd.wall, floor, mode, grid);
}

static bool writePPM(const FB& fb, const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", FBW, FBH);
    std::vector<uint8_t> row(static_cast<size_t>(FBW) * 3);
    for (int y = 0; y < FBH; ++y) {
        for (int x = 0; x < FBW; ++x) {
            uint32_t c = fb.px[static_cast<size_t>(y) * FBW + x];
            row[x * 3 + 0] = (c >> 16) & 0xFF;
            row[x * 3 + 1] = (c >> 8) & 0xFF;
            row[x * 3 + 2] = (c >> 0) & 0xFF;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

static std::string findRom(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') return argv[1];
    const char* cand[] = {
        "Zero Tolerance (USA, Europe) (Rev A).gen",
        "../Zero Tolerance (USA, Europe) (Rev A).gen",
        "../../Zero Tolerance (USA, Europe) (Rev A).gen",
    };
    for (auto c : cand) { std::ifstream f(c, std::ios::binary); if (f) return c; }
    return "";
}
static int argInt(int argc, char** argv, const char* key, int def) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::atoi(argv[i + 1]);
    return def;
}
static const char* argStr(int argc, char** argv, const char* key) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return nullptr;
}

#ifndef ZTPP_NO_SDL
#include <SDL.h>
// Прямоугольник аспекта contentAR, вписанный в окно по центру (леттербокс/пилларбокс).
static SDL_Rect fitRect(int winW, int winH, double contentAR) {
    double winAR = (winH > 0) ? (double)winW / winH : 1.0;
    int dw, dh;
    if (winAR > contentAR) { dh = winH; dw = (int)(winH * contentAR + 0.5); }  // окно шире → поля по бокам
    else                   { dw = winW; dh = (int)(winW / contentAR + 0.5); }  // окно выше → поля сверху/снизу
    return SDL_Rect{(winW - dw) / 2, (winH - dh) / 2, dw, dh};
}
// SRC (что копируем из fb) + DST (куда в окне). Вариант A: в игре презентуем ТОЛЬКО регион игры (без
// запечённых полос) → 4:3 растягивается на всю высоту окна. В меню — весь fb (чтобы меню не обрезалось).
static void presentRects(int winW, int winH, bool menuOpen, SDL_Rect& src, SDL_Rect& dst) {
    if (menuOpen) { src = SDL_Rect{0, 0, FBW, FBH}; dst = fitRect(winW, winH, (double)FBW / FBH); return; }
    src = SDL_Rect{g_viewX, g_viewY, g_viewW, g_viewH};                  // только регион игры
    double ar = (presentAspect() == 2) ? ((winH > 0) ? (double)winW / winH : 1.0)  // Stretch
              : (presentAspect() == 0) ? (4.0 / 3.0)                              // 4:3 (ЭЛТ)
                                       : (double)g_viewW / g_viewH;               // 1:1 (квадратный пиксель)
    dst = fitRect(winW, winH, ar);
}
inline void applyFullscreen(SDL_Window* w) {
    SDL_SetWindowFullscreen(w, presentFullscreen() ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
#endif

int main(int argc, char** argv) {
    std::string romPath = findRom(argc, argv);
    if (romPath.empty()) {
        std::fprintf(stderr, "ROM не найден. Запуск: ztpp <path-to-rom.gen> [--dump out.ppm]\n");
        return 1;
    }
    Rom rom;
    if (!rom.load(romPath)) { std::fprintf(stderr, "Не удалось прочитать ROM: %s\n", romPath.c_str()); return 1; }
    std::printf("ROM: %s (%zu байт)\n", romPath.c_str(), rom.size());

    // РЕНДЕР-МАСШТАБ (до создания FB/окна): внутренний fb = 640×RS. Из ini (раннее чтение) + override --rscale.
    presentRenderScale() = loadRenderScaleEarly("ztpp_settings.ini");
    { int rs = argInt(argc, argv, "--rscale", presentRenderScale()); presentRenderScale() = rs < 1 ? 1 : (rs > 3 ? 3 : rs); }
    FBW = FBH = 640 * presentRenderScale();

    // ЭТАП 0: вся игровая модель — в GameData; адреса ROM только в loadGameDataFromRom.
    // (Будущее: loadGameDataFromOztd для модов — порт об источнике не знает.)
    GameData gd;
    if (!loadGameDataFromRom(gd, rom))
        std::fprintf(stderr, "ВНИМАНИЕ: ZMAP-сигнатура E1 не совпала — данные могут быть неверны\n");
    pickupHiddenFn() = pickupIsConsumed;   // рендер пикапов: скрывать подобранные клетки (хук в raycaster)
    g_uiFont = gd.font.have ? &gd.font : nullptr;   // настоящий ZT-шрифт для drawText (иначе public-domain)
    g_uiFontBig = gd.fontBig.have ? &gd.fontBig : nullptr;  // Font_grph 8×16 для меню/настроек/заголовков

    // Фоны: E1=КОСМОС, E2=ГОРОД, E3=КОСМОС (дизасм 0x105e, выбор по эпизоду). Скролл по углу в рендере.
    int ep    = argInt(argc, argv, "--ep", 1) - 1;   if (ep < 0 || ep >= gd.episodes()) ep = 0;
    int floor = argInt(argc, argv, "--floor", 0);    if (floor < 0 || floor >= Level::FLOORS) floor = 0;
    int mode  = argInt(argc, argv, "--mode", 3);     if (mode < 0 || mode > 3) mode = 3; // ТОЛЬКО шутер (mode 3); прежние карта-режимы убраны, карта — по TAB (пауза)
    bool mapOpen = false;                            // ПАУЗА-КАРТА (по TAB): полная карта уровня + позиция игрока
    double mouseTurn = 0;                             // аккумулятор поворота мышью (применяется в движении, БЕЗ инерции)
    activeBg() = gd.bgForEpisode(ep);
    bool grid = true;

    FB fb;
    Camera cam;  cam.floor = floor;
    MetaCache meta; meta.wall = &gd.wall; meta.obj = &gd.obj; meta.shadeRampData = gd.shadeRamps.data();
    meta.fcTemplateData = gd.fcTemplates.data();   // слой пол/потолок (ROM-шаблоны по env)
    WallAnimator wallAnim;                          // анимация текстур стен (мигание экранов/ламп/глаз)
    std::vector<double> zbuf;
    Inventory inv;                  // инвентарь игрока (старт: пусто = кулаки, current=−1)
    int aEp = -1, aFloor = -1;      // эпизод/этаж, для которого заспавнены враги-актёры (респавн при смене)
    bool fullInv = false;           // V: тест-режим «всё оружие + неограниченный инвентарь» (Z/X листают карусель)
    Inventory savedInv;             // сохранённый инвентарь до входа в fullInv (восстанавливается на выходе)
    HudMessages msgs;               // HUD-сообщения как в ZT (очередь, нижний-левый угол, 4×9, Letters)
    int msgFloor = cam.floor;       // для детекта смены этажа (вверх/вниз)
    int prevAlive = -1;             // живых врагов в прошлом кадре (для «FLOOR SECURED»)
    int deathTimer = 0;             // показ «WASTED» после смерти (кадры)
    int spdMsg = 0;                 // показ «ENEMY SPEED» при регулировке (кадры)
    int lastHp = 100;               // HP прошлого кадра (детект пересечения порогов 50/15 для предупреждений)
    bool noclip = false;
    int lastCX = -1, lastCY = -1;   // клетка игрока (для триггера лифтов/лестниц по входу)
    // ── ВРЕМЕННО (пока дорабатываем reference-рендер): игра работает ТОЛЬКО в reference-режиме.
    //    DDA-рейкастер и фуллскрин-faithful отключены — клавиши F/R и кнопки меню Render/Reference
    //    заблокированы, режим форсится при старте и после загрузки настроек (перебивает ini).
    //    ВЕРНУТЬ ВСЕ РЕЖИМЫ — поставить REFERENCE_ONLY=false (одна строка). Код DDA/FPS не тронут.
    constexpr bool REFERENCE_ONLY = true;
    bool faithful = REFERENCE_ONLY ? true : (argInt(argc, argv, "--faithful", 1) != 0); // F — тумблер faithful/DDA
    bool reference = REFERENCE_ONLY ? true : (argInt(argc, argv, "--reference", 1) != 0); // R — референс-режим (дефолт ON, = ztpp_settings.ini)
    bool enemiesOn = (argInt(argc, argv, "--enemies", 1) != 0);  // настройка: спавн врагов на уровне (для отладки можно выкл.)
    gameMapMode() = (argInt(argc, argv, "--gamemap", 1) != 0);   // настройка: карта как в игре (vs классическая)

    auto respawn = [&]() { cam.floor = floor; meta.reset(&gd.levels[ep]); rcSpawn(cam, gd.levels[ep]); };
    respawn();
    wallAnim.init(gd.levels[ep], meta);   // живой texdef эпизода → MetaCache (анимация стен)

    // Тест-override позиции/угла камеры (для отладки граней): --px --py --ang(градусы)
    if (argStr(argc, argv, "--px")) cam.px = std::atof(argStr(argc, argv, "--px"));
    if (argStr(argc, argv, "--py")) cam.py = std::atof(argStr(argc, argv, "--py"));
    if (argStr(argc, argv, "--dir")) { double a = std::atof(argStr(argc, argv, "--dir")) * 3.14159265 / 180.0;
        cam.dirX = std::cos(a); cam.dirY = std::sin(a); cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66; }
    if (argStr(argc, argv, "--hstretch")) faHStretch() = std::atof(argStr(argc, argv, "--hstretch"));
    if (argStr(argc, argv, "--weapscale")) weaponScale() = std::atof(argStr(argc, argv, "--weapscale")); // тюнинг размера ствола
    if (argStr(argc, argv, "--decor"))  faDecor()  = (std::atoi(argStr(argc, argv, "--decor")) != 0); // вкл/выкл декор
    if (argStr(argc, argv, "--drawdist")) faDrawDist() = std::atoi(argStr(argc, argv, "--drawdist")); // отладка дальности (cull)
    if (argStr(argc, argv, "--stairk")) faStairK() = std::atof(argStr(argc, argv, "--stairk")); // калибровка крутизны скоса лестницы
    if (argStr(argc, argv, "--stairpitch")) faStairPitchOverride() = std::atof(argStr(argc, argv, "--stairpitch")); // статик-тест наклона
    if (argStr(argc, argv, "--stairuni")) faStairUni() = std::atof(argStr(argc, argv, "--stairuni")); // тест равномерного псевдопитча
    if (argStr(argc, argv, "--stairoff")) faStairOff() = (std::atoi(argStr(argc, argv, "--stairoff")) != 0); // диаг: откл. спец-лестницу
    if (argStr(argc, argv, "--ang")) {
        double a = std::atof(argStr(argc, argv, "--ang")) * 3.14159265 / 180.0;
        cam.dirX = std::cos(a); cam.dirY = std::sin(a);
        cam.planeX = -cam.dirY * 0.66; cam.planeY = cam.dirX * 0.66;
    }

    if (argStr(argc, argv, "--pitch")) cam.pitch = std::atof(argStr(argc, argv, "--pitch")); // отладка питча
    if (argStr(argc, argv, "--cabin")) { cam.cabin = std::atof(argStr(argc, argv, "--cabin")); cam.pitch = -cam.cabin; } // отладка перехода (лестница)
    if (argStr(argc, argv, "--elev"))  { cam.cabin = std::atof(argStr(argc, argv, "--elev")); cam.pitch = -cam.cabin; cam.elevState = 1; } // отладка лифта

    // --- Отладка: найти клетки заданного celltype во всех этажах эпизода (--scanct 0x56) ---
    if (const char* sc = argStr(argc, argv, "--scanct")) {
        int want = (int)std::strtol(sc, nullptr, 0);
        const Level& L = gd.levels[ep];
        std::printf("scanct эп%d ct=0x%02X:\n", ep + 1, want);
        for (int f = 0; f < Level::FLOORS; ++f)
            for (int y = 0; y < Level::H; ++y)
                for (int x = 0; x < Level::W; ++x)
                    if (L.cellType(f, x, y) == want)
                        std::printf("  этаж %2d  (%2d,%2d)\n", f, x, y);
        return 0;
    }
    // --- Отладка: сетка celltype вокруг клетки (--celldump "x y floor" [радиус --cdr]) ---
    if (const char* cd = argStr(argc, argv, "--celldump")) {
        int cx = 0, cy = 0, cf = floor; std::sscanf(cd, "%d %d %d", &cx, &cy, &cf);
        int r = argInt(argc, argv, "--cdr", 8);
        const Level& L = gd.levels[ep];
        std::printf("celldump эп%d этаж%d центр(%d,%d) r=%d  (#=стена, *=центр)\n     ", ep + 1, cf, cx, cy, r);
        for (int x = cx - r; x <= cx + r; ++x) std::printf("%3d", x);
        std::printf("\n");
        for (int y = cy - r; y <= cy + r; ++y) {
            std::printf("y%3d ", y);
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) { std::printf("  ."); continue; }
                uint8_t ct = L.cellType(cf, x, y);
                char mark = (x == cx && y == cy) ? '*' : (cellRenderWall(ct) ? '#' : ' ');
                std::printf("%c%02X", mark, ct);
            }
            std::printf("\n");
        }
        return 0;
    }
    // --- Отладка: трасса поездки лифта (--simelev "x y floor"), печатает cabin/floor/pitch по кадрам ---
    if (const char* se = argStr(argc, argv, "--simelev")) {
        int sx = 0, sy = 0, sf = 0; std::sscanf(se, "%d %d %d", &sx, &sy, &sf);
        Camera sc{}; sc.px = sx + 0.4; sc.py = sy + 0.5; sc.floor = sf; sc.dirX = 1; sc.dirY = 0;
        const Level& L = gd.levels[ep];
        std::printf("simelev старт (%d,%d) этаж %d, ct=0x%02X\n", sx, sy, sf, L.cellType(sf, sx, sy));
        // первый кадр со «входом в клетку» — триггер старта
        rcUpdateTransit(sc, L, true);
        for (int i = 0; i < 80; ++i) {
            bool busy = rcUpdateTransit(sc, L, false);
            if (i % 4 == 0 || sc.elevState == 0)
                std::printf("  k%2d: cabin=%+5.0f pitch=%+5.0f этаж=%2d elev=%+d %s\n",
                            i, sc.cabin, sc.pitch, sc.floor, sc.elevState, busy ? "ride" : "");
            if (sc.elevState == 0 && i > 2 && sc.cabin == 0.0) { std::printf("  СТОП на k%d, этаж=%d\n", i, sc.floor); break; }
        }
        return 0;
    }

    // --- Headless: дамп в PPM, без окна ---
    if (const char* dump = argStr(argc, argv, "--dump")) {
        { int di = argInt(argc, argv, "--dooriter", 15);   // степень открытия дверей в дампе (отладка)
          if (mode == 3) for (int i = 0; i < di; ++i) rcUpdateDoors(cam.floor, cam.px, cam.py, gd.levels[ep]); }
        for (int i = 0, af = argInt(argc, argv, "--animframe", 0); i < af; ++i)
            wallAnim.update();                              // прокрутить N игр.кадров анимации стен (отладка)
        decorFrame() = argInt(argc, argv, "--animframe", 0);  // фаза анимации декора (вентилятор/мигание)
        if (const char* w = argStr(argc, argv, "--weap")) {   // отладка: показать ствол N в руках
            int wi = (int)std::strtol(w, nullptr, 0);
            if (wi >= 1 && wi < 15) { inv.addItem(wi); inv.ammo[wi] = ITEMS[wi].ammoPickup;
                                      inv.sel = (int)inv.carried.size() - 1; inv.syncCurrent(); }
            else inv.current = -1;
        }
        if (argStr(argc, argv, "--fullinv")) {                // отладка: весь инвентарь (неогранич.) для HUD-карусели
            inv.unlimited = true;
            for (int i = 1; i < 15; ++i) { inv.addItem(i); inv.ammo[i] = 99; }
            inv.sel = argInt(argc, argv, "--sel", 0); inv.syncCurrent();
        }
        if (const char* fr = argStr(argc, argv, "--fire")) inv.fire = std::atoi(fr);  // отладка: кадр выстрела 1..4
        if (const char* hp = argStr(argc, argv, "--hp")) player().hp = std::atoi(hp);  // отладка: HP игрока (полоска)
        if (argStr(argc, argv, "--mapids")) mapShowIds() = true;        // карта: показать cell ID (для дампа)
        if (argStr(argc, argv, "--god")) player().godmode = true;       // отладка: бессмертие в дампе
        if (argStr(argc, argv, "--freeze")) enemiesFrozen() = true;     // отладка: враги замерли в дампе
        if (mode == 3 && enemiesOn) spawnEnemiesFromLevel(gd.levels[ep], floor);   // враги этажа (для дампа/отладки)
        if (argStr(argc, argv, "--desttest")) {                        // отладка: разрушить все разруш./секрет-клетки этажа
            Level& L = gd.levels[ep]; int n = 0, sx = -1, sy = -1; uint8_t sct = 0, sid = 0;
            for (int yy = 0; yy < Level::H; ++yy) for (int xx = 0; xx < Level::W; ++xx) {
                uint8_t ct = L.cellType(floor, xx, yy);
                if (wallIsDestructible(ct)) { if (sx < 0) { sx = xx; sy = yy; sct = ct; sid = L.cellId(floor, xx, yy); } requestDestruct(floor, xx, yy); ++n; }
            }
            applyDestruct(L);
            if (sx >= 0) std::printf("desttest: %d разруш.клеток; пример (%d,%d): cellId %d→%d, celltype 0x%02X→0x%02X\n",
                                     n, sx, sy, sid, L.cellId(floor, sx, sy), sct, L.cellType(floor, sx, sy));
        }
        if (argStr(argc, argv, "--shootray")) { double hx, hy; damageRay(gd.levels[ep], floor, cam.px, cam.py, cam.dirX, cam.dirY, 10, hx, hy);
            std::printf("shootray: hit (%.2f,%.2f), очередь разрушения=%zu\n", hx, hy, destructQueue().size()); }
        if (argStr(argc, argv, "--shoot")) fireSpawn(inv, gd.levels[ep], cam);        // отладка: выстрел
        for (int i = 0, n = argInt(argc, argv, "--shoot", 0); i < n; ++i) updateActors(gd.levels[ep], cam);
        if (mode == 3) { updateActors(gd.levels[ep], cam); applyDestruct(gd.levels[ep]); }  // 1 тик: спрайты + разрушение (для дампа)
        if (argStr(argc, argv, "--probe")) { int cx = (int)(cam.px + cam.dirX * 0.7), cy = (int)(cam.py + cam.dirY * 0.7);
            std::printf("probe (%d,%d): cellId %d celltype 0x%02X\n", cx, cy, gd.levels[ep].cellId(floor, cx, cy), gd.levels[ep].cellType(floor, cx, cy)); }
        render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
        if (argStr(argc, argv, "--pause")) drawPauseMap(fb, gd, ep, floor, cam);  // отладка: дамп меню паузы (Tab)
        if (const char* tm = argStr(argc, argv, "--testmsg")) {   // отладка: HUD-сообщение в дампе
            HudMessages dmsg;
            int id = std::atoi(tm); if (id > 0) dmsg.pushItem(id);
            dmsg.push(ztmsg::FLOOR_SECURED); dmsg.push(ztmsg::AMMO_LOW);
            dmsg.showFrames = 999; dmsg.update();
            dmsg.draw(fb, 14, FBH - 188, 2);
        }
        if (argStr(argc, argv, "--menu"))   // отладка: наложить меню настроек
            drawMenu(fb, 0.07, 0.04, faHStretch(), 60, 1.0, 0, faithful, noclip, reference, true, true, "Saved to ztpp_settings.ini");
        if (!writePPM(fb, dump)) { std::fprintf(stderr, "Не удалось записать %s\n", dump); return 1; }
        std::printf("Записано %s (эп %d, этаж %d, режим %d)\n", dump, ep + 1, floor, mode);
        return 0;
    }

#ifdef ZTPP_NO_SDL
    std::fprintf(stderr, "Собрано без SDL. Используйте --dump out.ppm\n");
    return 1;
#else
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) { std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    snd::load(rom);                                  // загрузка PCM-сэмплов + открытие аудио-устройства
    // РАЗМЕР ОКНА НЕЗАВИСИМ от внутреннего fb (640×RS): окно — нормального размера по дисплею, fb рендерится
    // внутри и ужимается SDL в окно (при RS>1 = суперсэмплинг → cell ID/оверлеи мельче и чётче). Не привязывать к FBW!
    int winSide = 720;
    { SDL_DisplayMode dm; if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        winSide = std::min(dm.w, dm.h) - 80; if (winSide < 480) winSide = 480; if (winSide > 1000) winSide = 1000; } }
    SDL_Window* win = SDL_CreateWindow("ztpp — Zero Tolerance (prototype)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, winSide, winSide, SDL_WINDOW_RESIZABLE);
    // без vsync — лимит кадров регулируем вручную (настройка 5..60)
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, FBW, FBH);

    const char* modeName[] = {"celltype", "textured", "wall-atlas", "fps-3d"};
    bool running = true, dirty = true;
    double moveSpd = 0.102875, turnSpd = 0.074; // настраиваемые: O/P движение, K/L вращение (дефолты = ztpp_settings.ini)
    int frameLimit = 15;                        // лимит кадров (5..60), регулируется в меню

    // --- Настройки: автозагрузка из файла (меню ESC сохраняет туда же) ---
    const char* CFG_PATH = "ztpp_settings.ini";
    { double st = faHStretch(); bool gm = gameMapMode();
      if (loadSettings(CFG_PATH, moveSpd, turnSpd, st, frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gm)) {
          faHStretch() = st; gameMapMode() = gm;
          if (frameLimit < 5) frameLimit = 5; if (frameLimit > 60) frameLimit = 60;
          std::printf("Настройки загружены из %s\n", CFG_PATH);
      } else {
          // Файла нет — сразу создаём его с настройками по умолчанию (значения из инициализации
          // переменных выше + дефолты геттеров), чтобы он существовал с первого запуска.
          saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(),
                       faithful, noclip, reference, enemiesOn, gameMapMode());
          std::printf("Создан %s с настройками по умолчанию\n", CFG_PATH);
      } }
    if (REFERENCE_ONLY) { faithful = true; reference = true; }   // ВРЕМЕННО: держим reference-режим (перебиваем ini)
    applyFullscreen(win);                         // применить сохранённый режим фуллскрина
    msgs.showFrames = frameLimit * 2;           // HUD-сообщение держится ~2 сек (масштаб по fps)
    bool menu = false;                          // меню настроек открыто (ESC)
    int menuPage = 0;                            // страница меню настроек (0..NPAGE-1)
    char menuStatus[64] = "";
    auto clampd = [](double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); };

    auto setFloor = [&](int nf) {
        if (nf < 0 || nf >= Level::FLOORS) return;
        floor = nf; dirty = true;
        if (mode == 3) respawn();
    };
    auto setEp = [&](int ne) { if (ne < 0 || ne >= gd.episodes()) return; ep = ne; activeBg() = gd.bgForEpisode(ep); dirty = true; if (mode == 3) respawn(); wallAnim.init(gd.levels[ep], meta); inv.reset(); fullInv = false; resetPlayerHP(); rcResetPickups(); clearActors(); clearWallState(); aEp = aFloor = -1; };  // новый эпизод → инвентарь/HP/актёры/стены с нуля
    auto setMode = [&](int nm) { mode = nm; dirty = true; if (mode == 3) respawn(); };

    // ── РЕГИСТРАЦИЯ КОНСОЛЬНЫХ КОМАНД (захват состояния игры) ──
    {
        // имя→id предмета (оружие + предметы). Алиасы для удобства.
        static const std::vector<std::pair<const char*, int>> ITEMID = {
            {"scanner",1},{"bioscanner",1},{"mine",2},{"vest",3},{"fireext",4},{"extinguisher",4},
            {"suit",5},{"flashlight",6},{"grenade",7},{"handgun",8},{"pistol",8},{"nightvision",9},
            {"laser",10},{"rocket",11},{"rocketlauncher",11},{"shotgun",12},{"flamethrower",13},{"flame",13},{"pulse",14},{"pulselaser",14}};
        static const std::vector<std::pair<const char*, uint8_t>> ENEMYID = {
            {"sgt",0x29},{"sergeant",0x29},{"fh",0x2A},{"formerhuman",0x2A},{"imp",0x2B},{"hydaca",0x65},
            {"revenant",0x66},{"boss1",0x67},{"dog",0x68},{"fhsf",0x69},{"boss3",0x6A},{"boss2",0x6B}};
        auto findItem = [](const std::string& s)->int { std::string n=s; for(auto&c:n)c=(char)tolower((unsigned char)c);
            for (auto& p : ITEMID) if (n == p.first) return p.second; return -1; };
        auto giveItem = [&](int id){ if (id<1||id>=15) return; if (!inv.has(id)) inv.addItem(id);
            inv.owned[id]=true; inv.ammo[id]=ITEMS[id].ammoCap; inv.syncCurrent(); };

        con::registerCmd("give", "give <handgun|shotgun|all|weapons|items|ammo|...> - give weapon/item", [&,findItem,giveItem](const std::vector<std::string>& a){
            std::string what = a.empty() ? "all" : a[0]; for(auto&c:what)c=(char)tolower((unsigned char)c);
            if (what=="all")     { for(int i=1;i<15;++i) giveItem(i); con::log("gave all weapons and items"); }
            else if (what=="weapons"){ for(int i=1;i<15;++i) if(ITEMS[i].weapon) giveItem(i); con::log("gave all weapons"); }
            else if (what=="items")  { for(int i=1;i<15;++i) if(!ITEMS[i].weapon) giveItem(i); con::log("gave all items"); }
            else if (what=="ammo")   { for(int i=1;i<15;++i) if(inv.has(i)) inv.ammo[i]=ITEMS[i].ammoCap; con::log("ammo refilled"); }
            else { int id=findItem(what); if(id>0){ giveItem(id); con::log(std::string("gave: ")+ITEMS[id].name); } else con::log("unknown item: "+what); }
        });
        con::registerCmd("spawn", "spawn <imp|fh|sgt|revenant|dog|hydaca|boss1..3|fhsf> [n] - spawn enemy ahead", [&](const std::vector<std::string>& a){
            if (a.empty()) { con::log("usage: spawn <enemy> [count]"); return; }
            std::string n=a[0]; for(auto&c:n)c=(char)tolower((unsigned char)c);
            uint8_t ct=0; for(auto&p:ENEMYID) if(n==p.first){ ct=p.second; break; }
            if(!ct){ con::log("unknown enemy: "+n); return; }
            int cnt = a.size()>1 ? std::atoi(a[1].c_str()) : 1; if(cnt<1)cnt=1; if(cnt>20)cnt=20;
            for(int i=0;i<cnt;++i){ double off=(i-cnt/2)*0.6;
                spawnEnemyByType(cam.floor, cam.px+cam.dirX*1.8 - cam.dirY*off, cam.py+cam.dirY*1.8 + cam.dirX*off, ct); }
            con::log("spawned "+std::to_string(cnt)+"x "+n);
        });
        con::registerCmd("clearinv","clearinv - clear inventory (back to fists)", [&](const std::vector<std::string>&){ inv.reset(); con::log("inventory cleared"); });
        con::registerCmd("playsnd","playsnd <n> - play PCM/DAC sample #n (find SFX index)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("PCM samples: "+std::to_string(snd::count()));return;} int n=std::atoi(a[0].c_str()); snd::play(n); con::log("play PCM "+std::to_string(n)+"/"+std::to_string(snd::count())); });
        con::registerCmd("playfm","playfm <patch> [note] - play FM patch as a note (find synth SFX)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("FM patches: "+std::to_string(snd::patchCount()));return;} int p=std::atoi(a[0].c_str()); int nt=a.size()>1?std::atoi(a[1].c_str()):60; snd::playFm(p,nt); con::log("play FM patch "+std::to_string(p)+" note "+std::to_string(nt)+"/"+std::to_string(snd::patchCount())); });
        con::registerCmd("playsfx","playsfx <id> - play game SFX by ID (ZT table @0xc5eb4; e.g. 0x1b=fire,0x6b=switch)", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("SFX ids: "+std::to_string(snd::sfxCount()));return;} int id=(int)std::strtol(a[0].c_str(),nullptr,0); bool ok=snd::playSfx(id); con::log("SFX 0x"+std::to_string(id)+(ok?" played":" (music/empty)")); });
        con::registerCmd("sfxtr","sfxtr <n> - transpose FM-SFX notes by n semitones (tune pitch; e.g. 12/-12)", [&](const std::vector<std::string>& a){ if(!a.empty()) snd::sfxTranspose()=std::atoi(a[0].c_str()); con::log("FM-SFX transpose = "+std::to_string(snd::sfxTranspose())); });
        con::registerCmd("god",    "god - toggle invulnerability", [&](const std::vector<std::string>&){ player().godmode=!player().godmode; con::log(std::string("god ")+(player().godmode?"ON":"OFF")); });
        con::registerCmd("noclip", "noclip - toggle walk through walls", [&](const std::vector<std::string>&){ noclip=!noclip; con::log(std::string("noclip ")+(noclip?"ON":"OFF")); });
        con::registerCmd("kill",   "kill - kill all enemies on floor", [&](const std::vector<std::string>&){ int n=0; for(auto&e:actors()) if(e.active&&e.think==AT_ENEMY){ e.hp=-9999; e.hitT=1; e.vx=e.vy=0; ++n; } con::log("killed "+std::to_string(n)+" enemies"); });
        con::registerCmd("heal",   "heal [n] - restore health (full by default)", [&](const std::vector<std::string>& a){ int amt=a.empty()?player().maxHp:std::atoi(a[0].c_str()); player().hp=std::min(player().maxHp, player().hp+amt); con::log("HP="+std::to_string(player().hp)); });
        con::registerCmd("freeze", "freeze - toggle enemy freeze", [&](const std::vector<std::string>&){ enemiesFrozen()=!enemiesFrozen(); con::log(std::string("freeze ")+(enemiesFrozen()?"ON":"OFF")); });
        con::registerCmd("blood",  "blood <on|off> - toggle blood spatter (ZT 0x157ca)", [&](const std::vector<std::string>& a){ if(!a.empty()) faBlood()=(a[0]!="off"&&a[0]!="0"); con::log(std::string("blood ")+(faBlood()?"ON":"OFF")); });
        con::registerCmd("floor",  "floor <n> - teleport to floor", [&](const std::vector<std::string>& a){ if(a.empty()){con::log("floor="+std::to_string(floor));return;} setFloor(std::atoi(a[0].c_str())); con::log("floor="+std::to_string(floor)); });
        con::registerCmd("ep",     "ep <n> - change episode (1..)", [&](const std::vector<std::string>& a){ if(a.empty())return; setEp(std::atoi(a[0].c_str())-1); con::log("episode="+std::to_string(ep+1)); });
        con::registerCmd("tp",     "tp <x> <y> - teleport to cell", [&](const std::vector<std::string>& a){ if(a.size()<2){con::log("usage: tp x y");return;} cam.px=std::atof(a[0].c_str())+0.5; cam.py=std::atof(a[1].c_str())+0.5; con::log("tp"); });
        con::registerCmd("pos",    "pos - print player position", [&](const std::vector<std::string>&){ char b[96]; std::snprintf(b,sizeof(b),"ep%d floor%d  px=%.2f py=%.2f", ep+1, floor, cam.px, cam.py); con::log(b); });
        con::registerCmd("enemies","enemies <on|off> - enemy spawning", [&](const std::vector<std::string>& a){ if(!a.empty()) enemiesOn=(a[0]!="off"&&a[0]!="0"); con::log(std::string("enemies ")+(enemiesOn?"ON":"OFF")); if(!enemiesOn){for(auto&e:actors())if(e.think==AT_ENEMY)e.active=false;} else {clearActors(); aEp=aFloor=-1;} });
        con::log("Zero Tolerance port console - type 'help'. Open/close: backquote key");
    }

    while (running) {
        Uint32 frameStart = SDL_GetTicks();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_TEXTINPUT && con::isOpen()) {  // ВВОД ТЕКСТА в консоль (раскладко-зависимый — правильно для печати)
                con::onText(e.text.text);
            }
            else if (e.type == SDL_KEYDOWN) {
                // ⚠ РАСКЛАДКО-НЕЗАВИСИМО: используем ФИЗИЧЕСКИЙ scancode, НЕ keysym.sym (рус.раскладка ломала клавиши).
                SDL_Scancode key = e.key.keysym.scancode;
                if (con::isOpen()) {                          // КОНСОЛЬ открыта — все клавиши ей (кроме ` / Esc закрывают)
                    if (key == SDL_SCANCODE_GRAVE || key == SDL_SCANCODE_ESCAPE) con::toggle();
                    else con::onKey(key, (SDL_GetModState() & KMOD_SHIFT) != 0);
                    continue;
                }
                if (key == SDL_SCANCODE_GRAVE) { con::toggle(); SDL_StartTextInput(); continue; }  // ` — открыть консоль (GZDoom-style)
                if (key == SDL_SCANCODE_ESCAPE) {            // ESC — открыть/закрыть меню настроек
                    menu = !menu; menuStatus[0] = 0; dirty = true;
                } else if (key == SDL_SCANCODE_F11) {        // F11 — фуллскрин (работает и в меню, и в игре)
                    presentFullscreen() = !presentFullscreen(); applyFullscreen(win); dirty = true;
                } else if (menu) {
                    // в меню реагируем только на ESC (выше) — остальное мышью
                } else switch (key) {
                    case SDL_SCANCODE_Q: running = false; break;
                    case SDL_SCANCODE_1: setEp(0); break;
                    case SDL_SCANCODE_2: setEp(1); break;
                    case SDL_SCANCODE_3: setEp(2); break;
                    case SDL_SCANCODE_TAB: mapOpen = !mapOpen; dirty = true; break;   // ПАУЗА-КАРТА вкл/выкл (мир на паузе)
                    case SDL_SCANCODE_G: grid = !grid; dirty = true; break;
                    case SDL_SCANCODE_N: noclip = !noclip; dirty = true; break;        // debug noclip
                    case SDL_SCANCODE_J: if (mode == 3 && player().jumpY <= 0.0 && player().crouchY >= -1.0) player().jumpVel = 9.0; break;  // ПРЫЖОК
                    case SDL_SCANCODE_I: player().godmode = !player().godmode; break;   // ЧИТ: бессмертие
                    case SDL_SCANCODE_H: enemiesFrozen() = !enemiesFrozen(); break;     // ЧИТ: враги замерли
                    case SDL_SCANCODE_Y: { double v = enemySpeedScale() - 0.1; enemySpeedScale() = v < 0.2 ? 0.2 : v; spdMsg = 90; } break;  // враги МЕДЛЕННЕЕ
                    case SDL_SCANCODE_U: { double v = enemySpeedScale() + 0.1; enemySpeedScale() = v > 2.5 ? 2.5 : v; spdMsg = 90; } break;  // враги БЫСТРЕЕ
                    case SDL_SCANCODE_V: if (mode == 3) {     // тест: дать ВСЁ ОРУЖИЕ (не предметы/пикапы), карусель Z/X
                        for (int i = 1; i < 15; ++i) if (ITEMS[i].weapon) { if (!inv.has(i)) inv.addItem(i); inv.ammo[i] = ITEMS[i].ammoCap; }
                        inv.syncCurrent();
                    } break;
                    case SDL_SCANCODE_Z: if (mode == 3) { cycleWeapon(inv, -1); snd::ev(snd::SFX_SWITCH); } break;
                    case SDL_SCANCODE_X: if (mode == 3) { cycleWeapon(inv, +1); snd::ev(snd::SFX_SWITCH); } break;
                    case SDL_SCANCODE_F: if (!REFERENCE_ONLY) { faithful = !faithful; dirty = true; } break;     // faithful/DDA (ВРЕМЕННО заблок.)
                    case SDL_SCANCODE_R: if (!REFERENCE_ONLY) { reference = !reference; dirty = true; } break;    // референс-режим (ВРЕМЕННО заблок.)
                    case SDL_SCANCODE_MINUS:  case SDL_SCANCODE_KP_MINUS: { double v = faHStretch() - 0.1; faHStretch() = v < 0.5 ? 0.5 : v; dirty = true; } break;
                    case SDL_SCANCODE_EQUALS: case SDL_SCANCODE_KP_PLUS:  { double v = faHStretch() + 0.1; faHStretch() = v > 4.0 ? 4.0 : v; dirty = true; } break;
                    case SDL_SCANCODE_PERIOD: case SDL_SCANCODE_RIGHTBRACKET: setFloor(floor + 1); break;
                    case SDL_SCANCODE_COMMA:  case SDL_SCANCODE_LEFTBRACKET:  setFloor(floor - 1); break;
                    case SDL_SCANCODE_UP:   if (mode != 3) setFloor(floor + 1); break;
                    case SDL_SCANCODE_DOWN: if (mode != 3) setFloor(floor - 1); break;
                    case SDL_SCANCODE_O: moveSpd = moveSpd > 0.015 ? moveSpd - 0.01 : moveSpd; break;
                    case SDL_SCANCODE_P: moveSpd = moveSpd < 0.30  ? moveSpd + 0.01 : moveSpd; break;
                    case SDL_SCANCODE_K: turnSpd = turnSpd > 0.008 ? turnSpd - 0.005 : turnSpd; break;
                    case SDL_SCANCODE_L: turnSpd = turnSpd < 0.15  ? turnSpd + 0.005 : turnSpd; break;
                    case SDL_SCANCODE_SEMICOLON: {  // ';' — печать позиции камеры
                        double ang = std::atan2(cam.dirY, cam.dirX) * 180.0 / 3.14159265;
                        std::printf("POS ep=%d floor=%d px=%.3f py=%.3f ang=%.1f\n", ep + 1, floor, cam.px, cam.py, ang);
                        std::fflush(stdout);
                    } break;
                    default: break;
                }
            }
            else if (e.type == SDL_MOUSEMOTION) {            // ПОВОРОТ МЫШЬЮ (только в шутере; БЕЗ инерции — применяется прямо)
                if (mode == 3 && !menu && !con::isOpen() && !mapOpen) mouseTurn += e.motion.xrel;
            }
            else if (e.type == SDL_MOUSEWHEEL) {             // КОЛЁСО → выбор оружия (вверх/вниз); кнопки Z/X не трогаем
                if (mode == 3 && !menu && !con::isOpen() && !mapOpen) { cycleWeapon(inv, e.wheel.y > 0 ? -1 : 1); snd::ev(snd::SFX_SWITCH); }
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN && menu && e.button.button == SDL_BUTTON_LEFT) {
                int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
                SDL_Rect src, dst; presentRects(ww, wh, true, src, dst);  // меню → весь fb (тот же rect, что и блит)
                int mx = (dst.w > 0) ? (e.button.x - dst.x) * src.w / dst.w + src.x : 0;   // окно → координаты FB
                int my = (dst.h > 0) ? (e.button.y - dst.y) * src.h / dst.h + src.y : 0;
                double frac = 0;
                switch (menuHit(mx, my, menuPage, frac)) {
                    case MA_MV_DEC: moveSpd = clampd(moveSpd - 0.005, 0.015, 0.30); break;
                    case MA_MV_INC: moveSpd = clampd(moveSpd + 0.005, 0.015, 0.30); break;
                    case MA_MV_BAR: moveSpd = clampd(0.015 + frac * (0.30 - 0.015), 0.015, 0.30); break;
                    case MA_TN_DEC: turnSpd = clampd(turnSpd - 0.002, 0.008, 0.15); break;
                    case MA_TN_INC: turnSpd = clampd(turnSpd + 0.002, 0.008, 0.15); break;
                    case MA_TN_BAR: turnSpd = clampd(0.008 + frac * (0.15 - 0.008), 0.008, 0.15); break;
                    case MA_ST_DEC: faHStretch() = clampd(faHStretch() - 0.1, 0.5, 4.0); break;
                    case MA_ST_INC: faHStretch() = clampd(faHStretch() + 0.1, 0.5, 4.0); break;
                    case MA_ST_BAR: faHStretch() = clampd(0.5 + frac * (4.0 - 0.5), 0.5, 4.0); break;
                    case MA_FL_DEC: if (frameLimit > 5)  --frameLimit; break;
                    case MA_FL_INC: if (frameLimit < 60) ++frameLimit; break;
                    case MA_FL_BAR: frameLimit = (int)(5.5 + frac * (60 - 5)); if (frameLimit<5) frameLimit=5; if (frameLimit>60) frameLimit=60; break;
                    case MA_ES_DEC: enemySpeedScale() = clampd(enemySpeedScale() - 0.1, 0.2, 2.5); break;  // СКОРОСТЬ ВРАГОВ
                    case MA_ES_INC: enemySpeedScale() = clampd(enemySpeedScale() + 0.1, 0.2, 2.5); break;
                    case MA_ES_BAR: enemySpeedScale() = clampd(0.2 + frac * (2.5 - 0.2), 0.2, 2.5); break;
                    case MA_WA_DEC: wallAnimSlow() = clampd(wallAnimSlow() - 0.25, 1.0, 4.0); break;  // АНИМАЦИЯ СТЕН (множитель замедления)
                    case MA_WA_INC: wallAnimSlow() = clampd(wallAnimSlow() + 0.25, 1.0, 4.0); break;
                    case MA_WA_BAR: wallAnimSlow() = clampd(1.0 + frac * (4.0 - 1.0), 1.0, 4.0); break;
                    case MA_SK_DEC: faStairK() = clampd(faStairK() - 0.1, 0.0, 3.0); dirty = true; break;  // ЛЕСТНИЦА: скос
                    case MA_SK_INC: faStairK() = clampd(faStairK() + 0.1, 0.0, 3.0); dirty = true; break;
                    case MA_SK_BAR: faStairK() = clampd(frac * 3.0, 0.0, 3.0); dirty = true; break;
                    case MA_SD_DEC: faStairUni() = clampd(faStairUni() - 0.05, 0.0, 1.0); dirty = true; break;  // ЛЕСТНИЦА: спуск
                    case MA_SD_INC: faStairUni() = clampd(faStairUni() + 0.05, 0.0, 1.0); dirty = true; break;
                    case MA_SD_BAR: faStairUni() = clampd(frac * 1.0, 0.0, 1.0); dirty = true; break;
                    case MA_MS_DEC: mouseSensitivity() = clampd(mouseSensitivity() - 0.1, 0.0, 2.0); break;  // ЧУВСТВИТЕЛЬНОСТЬ МЫШИ
                    case MA_MS_INC: mouseSensitivity() = clampd(mouseSensitivity() + 0.1, 0.0, 2.0); break;
                    case MA_MS_BAR: mouseSensitivity() = clampd(frac * 2.0, 0.0, 2.0); break;
                    case MA_SND:    soundOn() = !soundOn(); break;                              // ЗВУК вкл/выкл
                    case MA_SV_DEC: soundVolume() = clampd(soundVolume() - 0.1, 0.0, 1.0); break;  // ГРОМКОСТЬ
                    case MA_SV_INC: soundVolume() = clampd(soundVolume() + 0.1, 0.0, 1.0); break;
                    case MA_SV_BAR: soundVolume() = clampd(frac, 0.0, 1.0); break;
                    case MA_PAGE:   menuPage = (menuPage + 1) % menu::NPAGE; break;   // следующая страница настроек
                    case MA_REND:   if (!REFERENCE_ONLY) faithful = !faithful; break;   // ВРЕМЕННО заблок. (только reference)
                    case MA_NOCLIP: noclip = !noclip; break;
                    case MA_REFERENCE: if (!REFERENCE_ONLY) reference = !reference; break;   // ВРЕМЕННО заблок. (только reference)
                    case MA_ENEMIES: enemiesOn = !enemiesOn;            // вкл/выкл врагов на уровне (для отладки)
                                     if (!enemiesOn) { for (auto& a : actors()) if (a.think == AT_ENEMY) a.active = false; }  // убрать существующих
                                     else { clearActors(); aEp = aFloor = -1; }   // принудит. респавн врагов этажа
                                     break;
                    case MA_MAP: gameMapMode() = !gameMapMode(); break; // переключить карту (игровая/классическая)
                    case MA_MAPIDS: mapShowIds() = !mapShowIds(); dirty = true; break;  // cell ID на карте
                    case MA_PAUSEFULLMAP: pauseFullMap() = !pauseFullMap(); dirty = true; break;  // TAB: меню паузы ↔ full map
                    case MA_ASPECT: presentAspect() = (presentAspect() + 1) % 3; break;       // 4:3 → 1:1 → stretch
                    case MA_FILTER: presentLinear() = !presentLinear(); break;                // nearest/linear
                    case MA_FULLSCREEN: presentFullscreen() = !presentFullscreen(); applyFullscreen(win); break;
                    case MA_PHYSICS:  playerPhysics() = !playerPhysics();          // ZT-инерция ⇄ свободное движение
                                      cam.turnVel = cam.fwdVel = cam.strafeVel = 0.0; break;
                    case MA_GAMEDIST: gameDistOctagonal() = !gameDistOctagonal(); break;  // октаг. ⇄ Евклид
                    case MA_INVUNLIM: inventoryUnlimited() = !inventoryUnlimited(); inv.unlimited = inventoryUnlimited(); break;  // безлимит инвентаря
                    case MA_RSCALE: presentRenderScale() = presentRenderScale() % 3 + 1;   // 1→2→3→1 (применится с перезапуска)
                                    saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode());
                                    std::snprintf(menuStatus, sizeof(menuStatus), "Render scale %dx — restart to apply", presentRenderScale()); break;
                    case MA_SAVE:   saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode());
                                    std::snprintf(menuStatus, sizeof(menuStatus), "Saved to %s", CFG_PATH); break;
                    case MA_QUIT:   running = false; break;
                    case MA_RESUME: menu = false; dirty = true; break;
                    default: break;
                }
            }
        }

        if (con::isOpen()) dirty = true;   // консоль открыта → перерисовывать (видеть ввод/лог) и в статичных режимах
        // ЗАХВАТ МЫШИ для поворота: только в активном шутере (в меню/консоли/карте — обычный курсор для кликов)
        SDL_SetRelativeMouseMode((mode == 3 && !menu && !con::isOpen() && !mapOpen && mouseSensitivity() > 0.0) ? SDL_TRUE : SDL_FALSE);

        if (menu) {
            // меню настроек: рисуем замороженный кадр сцены + оверлей меню (мир на паузе)
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            drawMenu(fb, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), menuPage, faithful, noclip, reference, enemiesOn, gameMapMode(), menuStatus);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            SDL_SetWindowTitle(win, "ztpp — SETTINGS  (ESC закрыть, клик мышью)");
        } else if (mode == 3) {
            // непрерывное движение по зажатым клавишам (скорости настраиваемые: O/P, K/L)
            static const Uint8 ZEROKS[SDL_NUM_SCANCODES] = {0};
            const Uint8* ks = (con::isOpen() || mapOpen) ? ZEROKS : SDL_GetKeyboardState(nullptr);  // консоль/пауза-карта → ввод заморожен
            const bool halveMove = (player().knockPitch < -1) || (player().crouchY < -1.0);  // нокдаун/присед ÷2
            // Движение разрешено всегда: в поездке лифта rcUpdateTransit центрирует кабину в середине
            // перехода, а у этажа отпускает — можно «выскочить» вперёд на промежуточный этаж.
            if (playerPhysics()) {
                // ZT-ФИЗИКА (DAB8): инерция поворота (разгон/выбег), раздельные вперёд/назад, стрейф.
                // Раскладка: W/S(↑/↓) = вперёд/назад · A/D = стрейф влево/вправо · ←/→ = поворот (всё независимо).
                bool fwd = ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP], back = ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN];
                rcMovePhysics(cam, gd.levels[ep], fwd, back,
                              ks[SDL_SCANCODE_A], ks[SDL_SCANCODE_D],          // стрейф ←/→
                              ks[SDL_SCANCODE_LEFT], ks[SDL_SCANCODE_RIGHT],   // поворот ←/→
                              /*runMod*/ false, halveMove, noclip);
            } else {
                // «КАРТА БЕЗ ФИЗИКИ»: мгновенная скорость (свободный облёт для навигации/отладки).
                const double mv = moveSpd * (halveMove ? 0.5 : 1.0), rot = turnSpd;
                const double sx = cam.dirY, sy = -cam.dirX;   // «влево» (исправлено направление стрейфа)
                if (ks[SDL_SCANCODE_W] || ks[SDL_SCANCODE_UP])   rcMove(cam, gd.levels[ep],  cam.dirX * mv,  cam.dirY * mv, noclip);
                if (ks[SDL_SCANCODE_S] || ks[SDL_SCANCODE_DOWN]) rcMove(cam, gd.levels[ep], -cam.dirX * mv, -cam.dirY * mv, noclip);
                if (ks[SDL_SCANCODE_A]) rcMove(cam, gd.levels[ep],  sx * mv,  sy * mv, noclip);
                if (ks[SDL_SCANCODE_D]) rcMove(cam, gd.levels[ep], -sx * mv, -sy * mv, noclip);
                if (ks[SDL_SCANCODE_LEFT])  rcRotate(cam, -rot);
                if (ks[SDL_SCANCODE_RIGHT]) rcRotate(cam,  rot);
                cam.turnVel = cam.fwdVel = cam.strafeVel = 0.0;   // сброс инерции (чтобы при возврате не «дёрнуло»)
            }
            // ПОВОРОТ МЫШЬЮ — ПРЯМОЙ (без инерции), поверх клавиатурного. xrel·sens·0.004 рад/px.
            if (mouseTurn != 0.0 && !con::isOpen() && !mapOpen) { rcRotate(cam, mouseTurn * mouseSensitivity() * 0.004); }
            mouseTurn = 0.0;
            // ОТСКОК от урона (d800: вектор от источника, затухание) — двигаем игрока с коллизией.
            { PlayerState& pl = player();
              if (pl.knockVx != 0.0 || pl.knockVy != 0.0) {
                  rcMove(cam, gd.levels[ep], pl.knockVx, pl.knockVy, noclip);
                  pl.knockVx *= 0.80; pl.knockVy *= 0.80;
                  if (std::fabs(pl.knockVx) < 0.004 && std::fabs(pl.knockVy) < 0.004) pl.knockVx = pl.knockVy = 0.0;
              } }
            // ПРЫЖОК / ПРИСЕД (питч = верт. положение камеры): прыжок — парабола импульса (ZT -0x71e0→-0x71e6,
            // импульс 9 / гравитация 2); присед — питч вниз пока держишь C (ZT вниз+A, -0x71e4=-0x10).
            { PlayerState& pl = player();
              if (pl.jumpVel != 0.0 || pl.jumpY > 0.0) {                  // в полёте: гравитация
                  pl.jumpY += pl.jumpVel; pl.jumpVel -= 2.0;
                  if (pl.jumpY <= 0.0) { pl.jumpY = 0.0; pl.jumpVel = 0.0; }   // приземление
              }
              double cTgt = (ks[SDL_SCANCODE_C] && pl.jumpY <= 0.0) ? -16.0 : 0.0;  // присед (держать C); не в прыжке
              if (pl.crouchY > cTgt) { pl.crouchY -= 4; if (pl.crouchY < cTgt) pl.crouchY = cTgt; }
              else if (pl.crouchY < cTgt) { pl.crouchY += 4; if (pl.crouchY > cTgt) pl.crouchY = cTgt; } }
            // ОГОНЬ: SPACE или ЛКМ. Стреляет ТЕКУЩИМ оружием (inv.current = carried[sel]); тратит патрон.
            bool mouseFire = !con::isOpen() && !mapOpen && (SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON(SDL_BUTTON_LEFT));
            if (ks[SDL_SCANCODE_SPACE] || mouseFire) {
                int wid = inv.current;
                bool isW    = (wid >= 0 && wid < 15 && ITEMS[wid].weapon);
                bool ammoOk = !isW || inv.ammo[wid] > 0;
                bool start  = (inv.fire == 0);                                   // начать выстрел
                bool autoRe = fireAuto(wid) && inv.fire >= 3;                    // авто-перестрел (лазер/огнемёт)
                if ((start || autoRe) && inv.slide <= 0 && ammoOk) {
                    if (isW) { --inv.ammo[wid];                                  // расход боезапаса
                        if (inv.ammo[wid] == 5 || inv.ammo[wid] == 0) msgs.push(ztmsg::AMMO_LOW); }  // «мало патронов»
                    if (wid < 0) inv.punchSide ^= 1;                            // кулаки: чередуем руку
                    inv.fire = 1;
                    fireSpawn(inv, gd.levels[ep], cam);
                    switch (wid) {                                              // ЗВУК выстрела по типу оружия
                        case 8:  snd::ev(snd::SFX_HANDGUN); break;
                        case 12: snd::ev(snd::SFX_SHOTGUN); break;
                        case 10: snd::ev(snd::SFX_LASER); break;
                        case 14: snd::ev(snd::SFX_PULSE); break;
                        case 11: snd::ev(snd::SFX_ROCKET); break;
                        case 7:  snd::ev(snd::SFX_GRENADE); break;
                        case 13: if (inv.fire == 1) snd::ev(snd::SFX_FLAME); break;
                        case 4:  if (inv.fire == 1) snd::ev(snd::SFX_FOAM); break;
                        default: break;
                    }
                    if (isW && inv.ammo[wid] <= 0) inv.dropItem(wid);           // боезапас кончился → оружие выбывает (ZT FUN_10b00)
                }
            }
            // Враги-актёры: ПРОКСИ-СПАВН (ZT 11×11): при смене этажа собираем маркеры, каждый кадр спавним близкие.
            if (ep != aEp || cam.floor != aFloor) {
                clearActors(); if (enemiesOn) collectEnemyMarkers(gd.levels[ep], cam.floor);
                spawnMapFires(gd.levels[ep], cam.floor);     // карта-огонь (celltype 0x18) → вечные AT_FIRE
                aEp = ep; aFloor = cam.floor;
                prevAlive = 0;
            }
            player().fireImmune = inv.has(5);                 // огнезащ.костюм → иммун. к огню (нужно и для рендера)
            if (!mapOpen) {                                   // ПАУЗА-КАРТА: мир заморожен (не обновляем актёров/спавн/разрушение)
            if (enemiesOn) updateEnemySpawns(gd.levels[ep], cam.floor, cam.px, cam.py);  // спавн врагов рядом с игроком
            // БРОНЯ из инвентаря (перед уроном): жилет (3) → пул брони = заряд·10 (поглощает урон, −10%/попадание).
            player().armor      = inv.has(3) ? inv.ammo[3] * 10 : 0;
            updateActors(gd.levels[ep], cam);                 // think всех актёров + очередь спрайтов (worldFx)
            if (applyDestruct(gd.levels[ep])) snd::ev(snd::SFX_WALL);   // разрушаемые/секрет-стены + ЗВУК разрушения (0x3b)
            if (inv.has(3)) {                                 // броня израсходовалась в уроне → вернуть в заряд жилета; 0 → жилет выбывает
                inv.ammo[3] = player().armor / 10;
                if (player().armor <= 0) inv.dropItem(3);
            }
            { int alive = aliveEnemies(cam.floor);            // этаж зачищен → ZERO ENEMIES + FLOOR SECURED (и спавн-маркеры кончились)
              if (prevAlive > 0 && alive == 0 && pendingSpawns().empty()) { msgs.push(ztmsg::ZERO_ENEMIES); msgs.push(ztmsg::FLOOR_SECURED); }
              prevAlive = alive; }
            // СМЕРТЬ: HP дошло до 0 → респаун на точке спавна этажа + сброс HP + перезаспавн врагов.
            if (player().dead) {
                resetPlayerHP();
                respawn();                                    // на точку спавна (клетка 0x77) текущего этажа
                clearActors(); if (enemiesOn) collectEnemyMarkers(gd.levels[ep], cam.floor);
                aEp = ep; aFloor = cam.floor; prevAlive = 0;
                deathTimer = frameLimit;                       // показ «WASTED» ~1 сек
            }

            // лифты (авто-поездка с иллюзией питча) и лестницы (наклон по ходьбе + своп на переходе)
            int ccx = (int)cam.px, ccy = (int)cam.py;
            bool entered = (ccx != lastCX || ccy != lastCY);
            int prevElev = cam.elevState;
            rcUpdateTransit(cam, gd.levels[ep], entered);
            if (cam.elevState != 0 && prevElev == 0) snd::ev(snd::SFX_ELEVATOR);   // ЗВУК старта поездки лифта (0x6f)
            cam.pitch += player().knockPitch + player().jumpY + player().crouchY;  // нокдаун + ПРЫЖОК (вверх) + ПРИСЕД (вниз) — верт. положение камеры (поверх лифт/лестница-питча)
            lastCX = (int)cam.px; lastCY = (int)cam.py;   // после возможной центровки в кабине
            if (cam.floor != floor) floor = cam.floor;    // синхрон заголовка (без respawn)
            if (cam.floor != msgFloor) {                  // смена этажа → STEPPING ONE FLOOR UP/DOWN (ZT: ↑ индекс=вниз)
                msgs.push(cam.floor > msgFloor ? ztmsg::FLOOR_DOWN : ztmsg::FLOOR_UP);
                msgFloor = cam.floor;
            }
            if (rcUpdateDoors(cam.floor, cam.px, cam.py, gd.levels[ep])) snd::ev(snd::SFX_DOOR);  // анимация дверей + ЗВУК открытия (0x6d)
            wallAnim.update();                              // анимация текстур стен (1 игр.кадр)
            ++decorFrame();                                 // анимация декора (вентилятор/мигание ламп)
            { bool wasUnlim = inv.unlimited; inv.unlimited = inventoryUnlimited();   // тумблер «безлимитный инвентарь»
              if (wasUnlim != inv.unlimited) inv.syncCurrent(); }                    // смена режима → кольцо меняет размер, пере-клампить sel
            { static double s_hpx = cam.px, s_hpy = cam.py;  // ФАКТИЧЕСКАЯ скорость игрока (для покачивания ствола ∝ скорости)
              double sp = std::hypot(cam.px - s_hpx, cam.py - s_hpy);
              s_hpx = cam.px; s_hpy = cam.py;
              updateHeld(inv, sp); }                         // анимация выезда/боба(∝скорость)/выстрела ствола

            int got = rcTryPickup(inv, gd.levels[ep], cam.floor, cam.px, cam.py);  // подбор пикапа под игроком
            if (got == PICK_MEDKIT)   { msgs.push(ztmsg::MEDIPACK); con::log("picked up: medipack"); snd::ev(snd::SFX_PICKUP); }   // медпак (+HP)
            else if (got >= 0)        { msgs.pushItem(got); con::log(std::string("picked up: ") + ITEMS[got].name); snd::ev(snd::SFX_PICKUP); }  // «<предмет> COLLECTED»
            int dropped = rcTryCorpsePickup(inv, cam);             // подбор оружия с трупа солдата (шаг на труп)
            if (dropped >= 0) { msgs.pushItem(dropped); con::log(std::string("looted: ") + ITEMS[dropped].name); snd::ev(snd::SFX_PICKUP); }  // «<оружие> COLLECTED»
            // предупреждения о здоровье (ZT текст.asm d952<0x32 / d970<0xf): по пересечению порогов 50/15
            { int hp = player().hp; if (hp > 0) {
                if (hp <= 15 && lastHp > 15)      msgs.push(ztmsg::HEALTH_CRITICAL);
                else if (hp <= 50 && lastHp > 50) msgs.push(ztmsg::HEALTH_LOW);
              } if (hp < lastHp) snd::ev(snd::SFX_HURT); lastHp = hp; }   // ЗВУК получения урона
            msgs.update();                                // тик очереди HUD-сообщений
            }   // !mapOpen (конец заморозки мира на пауза-карте)

            // выставить регион игры в fb (для аспект-блита И позиционирования оверлеев в видимой части)
            if (mapOpen) {   // ПАУЗА-КАРТА (TAB): меню паузы (хедер+статус) ИЛИ плоская «full map» (тумблер настроек)
                             g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH;
                             if (pauseFullMap()) drawFullMap(fb, gd, ep, floor, cam);
                             else                drawPauseMap(fb, gd, ep, floor, cam); }
            else if (reference) { g_viewW = HUD_W * 2; g_viewH = HUD_H * 2; g_viewX = (FBW - g_viewW) / 2; g_viewY = (FBH - g_viewH) / 2;
                             renderReference(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, inv); }
            else           { g_viewX = 0; g_viewY = 0; g_viewW = FBW; g_viewH = FBH;
                             renderFPStoFB(fb, gd, gd.levels[ep], gd.wallPal, cam, meta, zbuf, faithful, inv); }
            // ОВЕРЛЕИ — внутри РЕГИОНА ИГРЫ (g_viewX/Y..+W/H); текст ×US (render-scale), иначе мельчает
            const int VX = g_viewX, VY = g_viewY, VW = g_viewW, VH = g_viewH, US = uiScale();
            if (!mapOpen) {                                     // на пауза-карте HUD-оверлеи не нужны
            msgs.draw(fb, VX + 14*US, VY + VH - 188*US, 2*US);   // HUD-сообщения: низ-лево региона (ROM x16/y160), над оружием
            if (deathTimer > 0) { drawTextC(fb, VX + VW / 2, VY + 80*US, "WASTED", 0xFFFF4040u, 4*US); --deathTimer; }  // смерть
            { int cy = VY + 8*US;                            // индикаторы читов (отладка) — верх региона
              if (player().godmode)   { drawText(fb, VX + VW - 130*US, cy, "GOD MODE",  0xFF50FF50u, US); cy += 12*US; }
              if (enemiesFrozen())    { drawText(fb, VX + VW - 130*US, cy, "ENEMIES FROZEN", 0xFF50FF50u, US); cy += 12*US; }
              if (spdMsg > 0) { char sb[40]; std::snprintf(sb, sizeof sb, "ENEMY SPEED %.1f [Y/U]", enemySpeedScale());
                  drawText(fb, VX + VW - 190*US, cy, sb, 0xFFFFD050u, US); cy += 12*US; --spdMsg; } }
            if (fullInv) {                                  // индикатор тест-режима «неогранич. инвентарь»
                char vb[80]; int wv = inv.current;
                std::snprintf(vb, sizeof(vb), "FULL INV (%d/%d)  %s", (int)inv.carried.size(),
                              inv.unlimited ? 99 : inv.capacity, wv < 0 ? "FISTS" : ITEMS[wv].name);
                drawTextC(fb, VX + VW / 2, VY + 70*US, vb, 0xFF80E0FFu, 2*US);
                drawTextC(fb, VX + VW / 2, VY + 90*US, "[Z/X] select   [SPACE] fire   [V] exit", 0xFF80C0FFu, US);
            }
            }   // !mapOpen
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            char fmode[32];
            if (reference) std::snprintf(fmode, sizeof(fmode), "REFERENCE");
            else if (faithful) std::snprintf(fmode, sizeof(fmode), "faithful x%.1f", faHStretch());
            else          std::snprintf(fmode, sizeof(fmode), "DDA");
            char title[280];
            std::snprintf(title, sizeof(title),
                "ztpp FPS [%s] эп%d эт%d (%.1f,%.1f)%s  спид:%.2f пов:%.3f  [ESC · F · Z/X оружие · V просмотр · N]",
                fmode, ep + 1, floor, cam.px, cam.py, noclip ? " NOCLIP" : "", moveSpd, turnSpd);
            SDL_SetWindowTitle(win, title);
        } else if (dirty) {
            render(fb, gd, ep, floor, mode, grid, cam, meta, zbuf, faithful, reference, inv);
            con::draw(fb, FBW, FBH, presentRenderScale());   // оверлей консоли (поверх всего)
            SDL_UpdateTexture(tex, nullptr, fb.px.data(), FBW * sizeof(uint32_t));
            char title[200];
            std::snprintf(title, sizeof(title),
                "ztpp — эп %d  этаж %d/%d  режим: %s   [ESC меню · 1/2/3 эп · , . этаж · TAB режим · G]",
                ep + 1, floor, Level::FLOORS - 1, modeName[mode]);
            SDL_SetWindowTitle(win, title);
            dirty = false;
        }

        SDL_RenderClear(ren);
        SDL_SetTextureScaleMode(tex, presentLinear() ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);  // фильтр
        { int ww = FBW, wh = FBH; SDL_GetWindowSize(win, &ww, &wh);
          SDL_Rect src, dst; presentRects(ww, wh, menu, src, dst);   // аспект-корректный блит (4:3 / 1:1 / stretch)
          SDL_RenderCopy(ren, tex, &src, &dst); }
        SDL_RenderPresent(ren);
        // лимит кадров: спим до конца кадрового интервала (1000/frameLimit мс)
        Uint32 target = (Uint32)(1000 / (frameLimit < 5 ? 5 : frameLimit));
        Uint32 spent = SDL_GetTicks() - frameStart;
        if (spent < target) SDL_Delay(target - spent);
    }
    saveSettings(CFG_PATH, moveSpd, turnSpd, faHStretch(), frameLimit, enemySpeedScale(), faithful, noclip, reference, enemiesOn, gameMapMode()); // автосохранение при выходе
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
#endif
}
