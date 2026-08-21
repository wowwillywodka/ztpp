// ztpp — src/mapview.cpp: top-down виды карт (celltype/textured/atlas/пауза-карта/радар/миникарта).
// Вынесено из main.cpp. Публичные функции — в mapview.hpp; приватные блиттеры — static тут.
#include "mapview.hpp"

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
void renderMap(FB& fb, const Level& lvl, const Palette& wallPal, const WallBank& wall,
                      int floor, int mode, bool grid) {
    fb.clear(0xFF0A0A0Eu);
    const int cellpx = FBW / lvl.W; // 20
    uint8_t tile[32 * 32];
    for (int y = 0; y < lvl.H; ++y) {
        for (int x = 0; x < lvl.W; ++x) {
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
    (void)celltypeColor;
}

// ПАУЗА-КАРТА (по TAB, как меню паузы ZT — НЕ радар): полная карта уровня + позиция/направление игрока.
// ── ПЛОСКАЯ ПОЛНАЯ КАРТА (меню настроек «full map»): только автокарта этажа + позиция, без хедера ──
void drawFullMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam) {
    renderMap(fb, gd.levels[ep], gd.wallPal, gd.wall, floor, 0, true);
    const int cellpx = FBW / gd.levels[ep].W;
    int px = (int)(cam.px * cellpx), py = (int)(cam.py * cellpx);
    for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx)
        if (dx*dx + dy*dy <= 10) fb.put(px + dx, py + dy, 0xFF40FF40u);
    for (int i = 0; i < 12; ++i) fb.put(px + (int)(cam.dirX*i), py + (int)(cam.dirY*i), 0xFFFFFF00u);
}

static void putNative(FB& fb, int vpX, int vpY, double sc, int nx, int ny, uint32_t col) {
    int x0 = vpX + (int)(nx * sc), y0 = vpY + (int)(ny * sc);
    int x1 = vpX + (int)((nx + 1) * sc), y1 = vpY + (int)((ny + 1) * sc);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) fb.put(x, y, col);
}

static void blitJuneMapSprite(FB& fb, const std::array<uint8_t, 64>& spr, const Palette& pal,
                              int vpX, int vpY, double sc, int nx, int ny) {
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c) {
            uint8_t idx = spr[(size_t)r * 8 + c] & 15;
            if (!idx) continue;
            putNative(fb, vpX, vpY, sc, nx + c, ny + r, pal.c[idx]);
        }
}

static void drawBztJunePauseMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam) {
    const Level& lvl = gd.levels[ep];
    if (lvl.floors <= 0) return;
    int fl = floor; if (fl < 0) fl = 0; if (fl >= lvl.floors) fl = lvl.floors - 1;

    fb.clear(0xFF000000u);
    double sc = (double)FBW / 320.0; { double sy = (double)FBH / 224.0; if (sy < sc) sc = sy; }
    int vpW = (int)(320 * sc), vpH = (int)(224 * sc), vpX = (FBW - vpW) / 2, vpY = (FBH - vpH) / 2;
    if (!gd.bztMapBg.empty()) {
        for (int y = 0; y < vpH; ++y) {
            int ny = (int)(y / sc); if (ny > 223) ny = 223;
            const uint32_t* src = &gd.bztMapBg[(size_t)ny * 320];
            for (int x = 0; x < vpW; ++x) {
                int nx = (int)(x / sc); if (nx > 319) nx = 319;
                fb.put(vpX + x, vpY + y, src[nx]);
            }
        }
    }

    const int fw = lvl.fw[fl], fh = lvl.fh[fl];
    int sx = bztPauseMapBaseX(lvl, fl, cam.px) + bztPauseMapScrollX();
    int sy = bztPauseMapBaseY(lvl, fl, cam.py) + bztPauseMapScrollY();
    if (fw <= 32) sx = 0; else { if (sx < 0) sx = 0; if (sx > fw - 32) sx = fw - 32; }
    if (fh <= 32) sy = 0; else { if (sy < 0) sy = 0; if (sy > fh - 32) sy = fh - 32; }

    auto nib = [&](int x, int y) -> uint8_t {
        if (x < 0 || y < 0 || x >= fw || y >= fh) return 0;
        uint8_t ct = lvl.cellType(fl, x, y);
        uint8_t v = gd.bztMapLut[(size_t)ct];
        return (v < 8) ? v : 0;
    };

    static constexpr int MAP_X = 96;  // nametable fill starts at VRAM 0xC198: col 12, row 3.
    static constexpr int MAP_Y = 24;
    const Palette& mapPal = gd.hudIconPal; // map attrs are 0xE341+ (priority + palette line 3).
    const Palette& textPal = gd.pauseTextPal; // title printer uses 0x42xx attrs after CRAM load @0x54222.
    for (int ty = 0; ty < 16; ++ty)
        for (int tx = 0; tx < 16; ++tx) {
            uint8_t ids[4] = {
                nib(sx + tx * 2,     sy + ty * 2),
                nib(sx + tx * 2 + 1, sy + ty * 2),
                nib(sx + tx * 2,     sy + ty * 2 + 1),
                nib(sx + tx * 2 + 1, sy + ty * 2 + 1),
            };
            for (int q = 0; q < 4; ++q) {
                const auto& br = gd.bztMapBrick[(size_t)ids[q]];
                int ox = MAP_X + tx * 8 + (q & 1) * 4;
                int oy = MAP_Y + ty * 8 + (q >= 2 ? 4 : 0);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) {
                        uint8_t idx = br[(size_t)r * 4 + c] & 15;
                        if (!idx) continue;
                        putNative(fb, vpX, vpY, sc, ox + c, oy + r, mapPal.c[idx]);
                    }
            }
        }

    int mx = MAP_X + (int)((cam.px - sx) * 4.0) - 4;
    int my = MAP_Y + (int)((cam.py - sy) * 4.0) - 4;
    if (mx > MAP_X - 8 && mx < MAP_X + 128 && my > MAP_Y - 8 && my < MAP_Y + 128)
        blitJuneMapSprite(fb, gd.bztMapPlayerMark, mapPal, vpX, vpY, sc, mx, my);

    int gi = ep * 16 + fl;
    if (gi < 0) gi = 0;
    if (!gd.levelNames.empty() && gi >= (int)gd.levelNames.size()) gi = (int)gd.levelNames.size() - 1;
    std::string nm = (!gd.levelNames.empty() && gi >= 0) ? gd.levelNames[gi] : "";
    if (!nm.empty()) drawTextFontC(fb, vpX + (int)(160 * sc), vpY + (int)(189 * sc), nm.c_str(),
                                   textPal.c[1], sc > 1.5 ? 2 : 1, gd.fontAlt.have ? &gd.fontAlt : nullptr);
}

// ── МЕНЮ ПАУЗЫ (Tab) — как в оригинале: автокарта этажа на бумаге-PDA + СПИСОК ЭТАЖЕЙ с паролями.
void drawPauseMap(FB& fb, const GameData& gd, int ep, int floor, const Camera& cam, const std::vector<std::string>* lines) {
    if (gd.build == Build::BZT_June && !gd.bztMapBg.empty()) {
        (void)lines;
        drawBztJunePauseMap(fb, gd, ep, floor, cam);
        return;
    }
    // МЕНЮ ПАУЗЫ как в ОРИГИНАЛЕ: фон = руки держат карту-PDA (ROM @0x12DD06, gd.pauseBg 320×224),
    // на поверхность карты рисуется blueprint этажа + позиция игрока + список этажей/паролей сверху.
    const Level& lvl = gd.levels[ep];
    int us = uiScale();
    fb.clear(0xFF000000u);
    {
        double sc = (double)FBW/320.0; { double sy=(double)FBH/224.0; if (sy<sc) sc=sy; }   // вписать 320×224
        int vpW=(int)(320*sc), vpH=(int)(224*sc), vpX=(FBW-vpW)/2, vpY=(FBH-vpH)/2;
        if (gd.pauseBg.empty()) {                              // фон не вскрыт (ZTU) → тёмная бумага-подложка,
            fb.rect(vpX, vpY, vpW, vpH, 0xFF04040Au);          //   карта/строки рисуются как обычно ниже
        } else
        for (int y=0;y<vpH;++y){ int ny=(int)(y/sc); if(ny>223)ny=223;
            const uint32_t* src=&gd.pauseBg[(size_t)ny*320];
            for(int x=0;x<vpW;++x){ int nx=(int)(x/sc); if(nx>319)nx=319; fb.put(vpX+x, vpY+y, src[nx]); } }
        // КАРТА = РЕАЛЬНЫЕ ТАЙЛЫ ZT (ZT 0x2fa0: клетка celltype → LUT 0xe23e → пул 0x3372 → 4×4 кирпичик;
        // палитра CRAM линия 3; стены idx1 серый, скосы idx2, двери idx4/5 коричн./красный). Ассеты (mapres::)
        // сняты из VRAM-эталона MAME. Карта = 16×16 тайлов = 128×128 px на бумаге @экран(96,80), клетка=4×4 px.
        // ⭐ФОН idx0 = ПРОЗРАЧЕН (пол показывает БУМАГУ pauseBg сквозь себя, как VDP: idx0 = прозрачный; НЕ пурпур!).
        const int nx0=96, ny0=80, cellN=4; double px=sc; if(px<1)px=1;   // 1 px кирпичика = sc экранных
        auto fbx=[&](double gx){ return vpX+(int)((nx0+gx*cellN)*sc); }; auto fby=[&](double gy){ return vpY+(int)((ny0+gy*cellN)*sc); };
        for (int cy=0;cy<lvl.H;++cy) for (int cx=0;cx<lvl.W;++cx) {
            uint8_t ic = mapres::ICON[lvl.cellType(floor,cx,cy) & 0xFF];  // celltype → иконка-idx
            if (ic >= (uint8_t)mapres::NBRICK) ic = 0;
            const uint8_t* br = mapres::BRICK[ic];                        // 4×4 кирпичик
            for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
                uint8_t idx = br[r*4+c] & 15; if (idx==0) continue;       // idx0 = ПРОЗРАЧНО (пол → бумага сквозь)
                uint32_t col = mapres::PAL[idx];
                int ox=fbx(cx)+(int)(c*px), oy=fby(cy)+(int)(r*px);
                for(int yy=0;yy<(int)px+1;++yy)for(int xx=0;xx<(int)px+1;++xx) fb.put(ox+xx,oy+yy,col);
            }
        }
        // МАРКЕР ИГРОКА = КРАСНЫЙ КРЕСТИК (ZT тайл 0x19d, VDP-спрайт на позиции игрока), idx0 прозрачен
        double mpx=fbx(cam.px)-(int)(4*px), mpy=fby(cam.py)-(int)(4*px);   // 8×8 тайл центрирован
        for (int r=0;r<8;++r) for (int c=0;c<8;++c) {
            uint8_t v=mapres::MARKER[r*8+c]; if(v==0) continue;           // прозрачный фон крестика
            uint32_t col=mapres::PAL[v & 15];
            int ox=(int)(mpx+c*px), oy=(int)(mpy+r*px);
            for(int yy=0;yy<(int)px+1;++yy)for(int xx=0;xx<(int)px+1;++xx) fb.put(ox+xx,oy+yy,col);
        }
        // (ID-карта бойца — НЕ здесь: ROM 2acc рисует её на игровом КОКПИТЕ при загрузке уровня → renderReference)
        // ⭐ВЕРХ (ROM 5755e/5761e/57d2e): СПИСОК ЭТАЖЕЙ, до 3 видимых строк (nametable rows 1/3/5 → y=8/24/40),
        // центрирование; пройденный этаж = «<label 576aa> ПАРОЛЬ», текущий = «<label> *SECURED*/*NOT SECURED*».
        // Строки собирает main (пароли требуют инвентарь). ⚠РЕГИСТРО-КОРРЕКТНО (selFont): пароль CASE-SENSITIVE —
        //   Font_grph заглавил строчные («LFpb*TnUg»→«LFPB*TNUG») → ввод не совпадал → «неверный пароль».
        if (lines) {
            int psc = (int)(sc + 0.5); if (psc < 1) psc = 1;
            for (int li = 0; li < (int)lines->size() && li < 3; ++li) {
                const std::string& s = (*lines)[li];
                if (gd.selFont.have) {
                    int x = vpX + (int)(160*sc) - (int)s.size()*8*psc/2, y = vpY + (int)((8 + li*16)*sc);
                    for (char ch : s) {
                        unsigned uc = (unsigned char)ch;
                        if (uc >= 0x20 && uc <= 0x7f) { int e = (int)uc - 0x20;
                            for (int ry = 0; ry < 16; ++ry) for (int rx = 0; rx < 8; ++rx) {
                                uint8_t idx = gd.selFont.idx[e][ry*8+rx]; if (!idx) continue;
                                uint32_t col = gd.pauseTextPal.c[idx];
                                for (int sy = 0; sy < psc; ++sy) for (int sx = 0; sx < psc; ++sx) fb.put(x+rx*psc+sx, y+ry*psc+sy, col);
                            } }
                        x += 8*psc;
                    }
                } else drawTextBigC(fb, vpX + (int)(160*sc), vpY + (int)((8 + li*16)*sc), s.c_str(), 0, psc, false, gd.pauseTextPal.c.data());
            }
        }
        // НИЗ: короткое имя уровня шрифтом Font2 — цвет = idx1 палитры экранного текста
        // (ZT: 0x0444 → #575757; June: своя палитра Font_grph/options @0x54222).
        (void)us;
        int gi=ep*16+floor; if(gi<0)gi=0; if(!gd.levelNames.empty()&&gi>=(int)gd.levelNames.size())gi=(int)gd.levelNames.size()-1;
        std::string nm = (!gd.levelNames.empty()&&gi>=0)?gd.levelNames[gi]:"";
        if (!nm.empty()) drawTextFontC(fb, FBW/2, vpY+(int)(212*sc), nm.c_str(), gd.pauseTextPal.c[1],   // ⭐212: карта до 208 — не наезжает
                                       sc > 1.5 ? 2 : 1, gd.fontAlt.have ? &gd.fontAlt : nullptr);
    }
}

void renderAtlas(FB& fb, const WallBank& wall, const Palette& wallPal) {
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
void drawMinimap(FB& fb, const Level& lvl, const Camera& cam) {
    const int s = 5, ox = 8, oy = 8;
    fb.rect(ox - 2, oy - 2, lvl.W * s + 4, lvl.H * s + 4, 0xFF000000u);
    for (int y = 0; y < lvl.H; ++y)
        for (int x = 0; x < lvl.W; ++x) {
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
void drawMinimapRect(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                            const Level& lvl, const Camera& cam) {
    auto put = [&](int x, int y, uint32_t c) {
        if (x >= 0 && x < bw && y >= 0 && y < bh) buf[(size_t)y * bw + x] = c;
    };
    int s = std::min(rw / lvl.W, rh / lvl.H); if (s < 1) s = 1;
    int mw = lvl.W * s, mh = lvl.H * s;
    int ox = rx + (rw - mw) / 2, oy = ry + (rh - mh) / 2;
    for (int y = ry; y < ry + rh; ++y)                       // тёмная подложка на всю область радара
        for (int x = rx; x < rx + rw; ++x) put(x, y, 0xFF000000u);
    for (int y = 0; y < lvl.H; ++y)
        for (int x = 0; x < lvl.W; ++x) {
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

// РАДАР КОКПИТА (ZT FUN_0000e816) — РЕАЛЬНЫЙ ГРИД-ТАЙЛСЕТ (геймплей-дамп VRAM): окно 10×10 клеток ВОКРУГ игрока
// (скролл, центр=игрок, клам 5..27), каждая клетка = тайл 0x347+icon (icon = celltype→LUT 0xe23e = mapres::ICON):
// пол=чёрный квадрат с жёлто-оранж. РАМКОЙ-СЕТКОЙ, стена=сплошной жёлтый, диагонали=треугольники, двери=бары.
// Палитра radarres (CRAM линия 3 геймплея): idx7=жёлтый, idx6=оранж, idx4=коричн, idx15=чёрный, idx0=прозрачно.
// Маркер игрока = тайл 0x351 (маленький «+», жёлтый/оранж), НЕ стрелка. Враги (aggro / все со сканером) = крас.«+».
void drawGameMap(uint32_t* buf, int bw, int bh, int rx, int ry, int rw, int rh,
                        const Level& lvl, const Camera& cam, const GameData& gd, bool hasScanner) {
    (void)gd;
    auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && x < bw && y >= 0 && y < bh) buf[(size_t)y * bw + x] = c; };
    const int N = 10, s = rw / N;                                    // 10×10 окно, s px/клетка (=8·scale)
    const int bp = (s / 8 < 1) ? 1 : s / 8;                          // блок на 1 пиксель тайла 8×8
    for (int y = ry; y < ry + rh; ++y) for (int x = rx; x < rx + rw; ++x) put(x, y, radarres::PAL[15]);  // фон = чёрный
    int pcx = (int)cam.px, pcy = (int)cam.py;                        // клетка игрока
    int ox = pcx - N / 2, oy = pcy - N / 2;                          // окно центрировано на игроке…
    if (ox < 0) ox = 0; if (ox > lvl.W - N) ox = lvl.W - N;    // …с кламом к границам карты (как ZT: центр 5..27)
    if (oy < 0) oy = 0; if (oy > lvl.H - N) oy = lvl.H - N;
    for (int gy = 0; gy < N; ++gy)
        for (int gx = 0; gx < N; ++gx) {
            int mx = ox + gx, my = oy + gy;
            if (mx < 0 || my < 0 || mx >= lvl.W || my >= lvl.H) continue;
            uint8_t ct = lvl.cellType(cam.floor, mx, my);
            int px0 = rx + gx * s, py0 = ry + gy * s;
            if (mapShowIds()) {                                      // ДЕВ-РЕЖИМ: значок клетки + cell ID тонким шрифтом
                blitIconBuf(buf, bw, bh, cellIcon(ct), px0, py0, s);
                drawTinyHexByte(buf, bw, bh, px0 + (s - 7) / 2, py0 + (s - 5) / 2, lvl.cellId(cam.floor, mx, my), 0xFFFFFFFFu);
                continue;
            }
            uint8_t ic = mapres::ICON[ct]; if (ic >= 8) ic = 0;      // celltype → иконка 0..7
            // ОТКРЫТАЯ ДВЕРЬ → иконка ПОЛА (ZT: при открытии celltype 6/7 → 0x2D/0x2E → icon0; тайл радара
            // меняется с бара на клетку-пол, пока дверь открыта). Открытость в doorMap() (0..1).
            if ((ic == 6 || ic == 7) && doorOpen(cam.floor, mx, my) > 0.01) ic = 0;
            const uint8_t* t = radarres::TILE[ic];                   // реальный радар-тайл 8×8 (0x347+ic)
            for (int tr = 0; tr < 8; ++tr) for (int tc = 0; tc < 8; ++tc) {
                uint8_t idx = t[tr * 8 + tc]; if (idx == 0) continue;  // idx0 = прозрачно
                uint32_t col = radarres::PAL[idx & 15];
                for (int yy = 0; yy < bp; ++yy) for (int xx = 0; xx < bp; ++xx)
                    put(px0 + tc * bp + xx, py0 + tr * bp + yy, col);
            }
        }
    // Анимация маркеров (ZT: счётчик -$714a per-frame). pf = кадр пульса (ping-pong 0x34f-0x353), blink = трейл.
    static unsigned radarTick = 30; ++radarTick;                     // seed 30 → в дампе виден трейл/пульс
    int  pf    = radarres::PING[(radarTick / 4) & 7];                // кадр пульса (≈15Гц)
    bool blink = ((radarTick / 6) & 1) != 0;                         // мигание трейла (≈5Гц)
    // Блит тайла-маркера (8×8), центр «+» = тайл-коорд (2,2) → на экранной точке (scx,scy). idx0 прозрачно.
    auto blitTileM = [&](const uint8_t* tile, int scx, int scy) {
        int mx0 = scx - 2 * bp, my0 = scy - 2 * bp;
        for (int tr = 0; tr < 8; ++tr) for (int tc = 0; tc < 8; ++tc) {
            uint8_t idx = tile[tr * 8 + tc]; if (idx == 0) continue;
            uint32_t col = radarres::PAL[idx & 15];
            for (int yy = 0; yy < bp; ++yy) for (int xx = 0; xx < bp; ++xx)
                put(mx0 + tc * bp + xx, my0 + tr * bp + yy, col);
        }
    };
    // БЛИПЫ ВРАГОВ (ZT eb5a): показывает ВСЕХ активных врагов в радиусе окна — флаг 0x10 стоит при СПАВНЕ (objdef 0xdc),
    // НЕ по aggro/state (юзер: патрульный Revenant/Dog видны и без сканера). BIO SCANNER — ДОПОЛНИТЕЛЬНО дормантные (ещё не
    // заспавненные) маркеры этажа. Маркер = КРАСНЫЙ пульсирующий «+» (ZT тайл 0x354+). Живой = hp>0 (hp≤0 = труп/Boss3-претворство).
    auto blip = [&](double wx, double wy) {
        double gxp = (wx - ox), gyp = (wy - oy);
        if (gxp < 0 || gyp < 0 || gxp >= N || gyp >= N) return;
        blitTileM(radarres::ENEMY[pf], rx + (int)(gxp * s), ry + (int)(gyp * s));
    };
    for (const Actor& a : actors()) {
        if (!a.active || a.think != AT_ENEMY || a.floor != cam.floor || a.hp <= 0) continue;
        blip(a.x, a.y);                                              // все заспавненные/патрулирующие враги (без aggro-гейта)
    }
    if (hasScanner) for (const auto& m : pendingSpawns()) if (m.floor == cam.floor) blip(m.x + 0.5, m.y + 0.5);  // сканер: + дормантные маркеры ЭТОГО этажа
    // ИГРОК: пульсирующий «+» (0x34f-0x353) + МИГАЮЩИЙ ТРЕЙЛ направления — 3 крестика (тайл 0x34f) впереди по взгляду
    // на 0.5/1.0/1.5 клетки (ZT e9a8: dir·k/64, dir=cos/sin амплитуда 256 → 0.5кл·k). БЕЗ стрелки.
    double cxpF = (cam.px - ox) * s, cypF = (cam.py - oy) * s;
    if (blink) for (int k = 1; k <= 3; ++k)
        blitTileM(radarres::PULSE[0], rx + (int)(cxpF + cam.dirX * 0.5 * k * s),
                                      ry + (int)(cypF + cam.dirY * 0.5 * k * s));
    blitTileM(radarres::PULSE[pf], rx + (int)cxpF, ry + (int)cypF);    // сам маркер игрока (пульс)
}
