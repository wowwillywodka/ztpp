// ztpp — src/rom/enemy_sprites.hpp: СПРАЙТЫ ВРАГОВ (декод из ROM + рантайм-выбор направления/шейда).
// Дерево state→dir→frame по дизасму. Вынесено из gamedata.hpp. Заголовок = структуры + глобалы +
// ГОРЯЧИЕ хелперы (enemyDirIndex/enemyGfxSlot — на врага/кадр); тяжёлый декод (раз при старте) — в .cpp.
#pragma once
#include "rom.hpp"
#include "gfx.hpp"
#include <vector>
#include <cstdint>
#include <cmath>

// Кадр 0x26 б, тайл 32×32 col-major. idx = сырые пал.индексы 0..15 (для CLUT-шейда спрайтов как стены).
struct EnemySprite { int w = 0, h = 0, drawW = 0, drawH = 0; std::vector<uint32_t> argb; std::vector<uint8_t> idx; bool ok = false; };
// КАДРЫ ХОДЬБЫ (front, dir0) по слоту enemyGfxSlot (0..9) — совместимость; = g_enemyAnim[slot].walk[0].
inline std::vector<EnemySprite> g_enemyWalk[16];

// ── НАБОР АНИМАЦИЙ ВРАГА (draw-селекторы objdef+0x1c → таблица 0x1690E): ходьба по НАПРАВЛЕНИЯМ +
//    стрельба/удар + смерть. Направление по углу facing(скорость)↔игрок (ZT 0x1BA08). hflip запечён. ──
struct EnemyAnimSet {
    std::vector<EnemySprite> walk[6];            // anim ходьбы: до 6 направлений
    int walkDirs = 1;
    std::vector<EnemySprite> walkB[6];           // ВТОРАЯ вариация ходьбы (FH anim9)
    int walkBDirs = 0;
    bool hasVariant = false;                      // есть ли 2-я вариация (per-actor выбор по Actor::variant)
    std::vector<EnemySprite> fire, hit, death;   // стрельба/удар, стаггер, смерть (dir0)
    std::vector<EnemySprite> climb;               // Hydaca: ВЕРТИКАЛЬНЫЙ спрайт лазанья по стене (32×64) = a1 (падение вниз)
    std::vector<EnemySprite> climbDir[6];         // Hydaca: 6 directional вертик. поз лазанья
    std::vector<EnemySprite> fallUp;              // Hydaca: a8 = падение ВВЕРХ / срыв (ZT draw state2 $2e>0)
    std::vector<EnemySprite> fallDead;            // Hydaca: a10 = падение МЁРТВОЙ (ZT draw state2 HP<0)
    int climbDirs = 0;
    bool ok = false;
};
inline EnemyAnimSet g_enemyAnim[16];
inline EnemyAnimSet g_enemyAnimVar2[16];          // полный альт-набор по слоту (FH вар2); .ok=false если нет

// ВЫБОР НАПРАВЛЕНИЯ — ТОЧНО ПО ZT 0x1ba2a: U=(self−player), V=скорость(facing); d3=256·cos∠(U,V); cross=U×V.
//   ПРИБЛИЖЕНИЕ (V≈к игроку = −U) → d3≈−256 → **dir0 = ФРОНТ**. Бегство → d3≈+256 → dir1=спина.
inline int enemyDirIndex(double vx, double vy, double rx, double ry, int dirCount) {
    if (dirCount <= 1) return 0;
    double vl = std::hypot(vx, vy);
    if (vl < 1e-4) return 0;                              // стоит → dir0 (фронт)
    double ux = -rx, uy = -ry, ul = std::hypot(ux, uy);  // U = self−player
    double dot = ux * vx + uy * vy;                       // (self−player)·V
    if (dirCount == 2) return (dot >= 0) ? 1 : 0;         // ZT d7==2: dot≥0 (бегство)→dir1, иначе dir0(фронт)
    double d3 = 256.0 * dot / (ul * vl + 1e-6);           // 256·cos∠(U,V)
    // ⭐cross = ROM d4 (1ba9e): (posY−camY)·velX − (posX−camX)·velY = uy·vx − ux·vy. БЫЛО ux·vy−uy·vx = −d4 → лево/право
    // виды (dir2/3, dir4/5) ЗЕРКАЛИЛИСЬ (Revenant вбок смотрел не туда = «задом»). Теперь знак как в ROM.
    double cross = uy * vx - ux * vy;
    if (dirCount == 4) {                                  // ZT d7==4
        if (d3 >= 0x9b) return 1;                         // бегство → спина
        if (d3 <= -0x9b) return 0;                        // приближение → ФРОНТ
        return (cross < 0) ? 2 : 3;                       // бок
    }
    if (d3 >= 0x9b) return 1;                             // ZT d7==6: спина
    if (d3 <= -0xC8) return 0;                            // ФРОНТ (приближение, d3≤−200)
    if (d3 >= -0x5A) return (cross < 0) ? 2 : 3;          // бок (−90..155)
    return (cross < 0) ? 4 : 5;                           // боко-фронт (−200..−90)
}
// celltype → слот (порядок: Sgt/FH/Imp/Hydaca/Revenant/Boss1/dog/FH-SF/Boss3/Boss2).
inline int enemyGfxSlot(uint8_t ct) {
    switch (ct) { case 0x29: return 0; case 0x2A: return 1; case 0x2B: return 2; case 0x65: return 3;
        case 0x66: return 4; case 0x67: return 5; case 0x68: return 6; case 0x69: return 7;
        case 0x6A: return 8; case 0x6B: return 9; default: return -1; }
}

// ⭐CLUT-ШЕЙД СПРАЙТОВ (eda0/скейлер 0xf6xx: `move.b (a3,d1.w),(a0)`, a3 = 0x10d1be + band·0x100). 12 бандов
// × 256 б: индекс = байт (2 текселя 4bpp), выход = шейд-байт. ТОТ ЖЕ приём, что стены (CLUT 0x3392).
struct SpriteClut {
    uint8_t band[12][256] = {};
    bool ok = false;
    void load(const Rom& rom);                    // enemy_sprites.cpp
};
inline SpriteClut& g_spriteClut() { static SpriteClut c; return c; }

// Наполнить g_enemyAnim/g_enemyAnimVar2/g_enemyWalk + CLUT спрайтов из ROM (дерево 0x1B7B38…, пал 0x20F2).
void decodeEnemySprites(const Rom& rom, const Palette& pal);
