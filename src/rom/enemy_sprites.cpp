// ztpp — src/rom/enemy_sprites.cpp: декод дерева спрайтов врагов из ROM (раз при старте).
// Приватные anim-таблицы (по разбору per-enemy draw-fn) + декодеры. Публичный интерфейс — enemy_sprites.hpp.
#include "enemy_sprites.hpp"

// {walk, fire, hit, death} anim-индексы по слоту (−1 = нет → fallback ходьба). slot: 0Sgt 1FH 2Imp 3Hydaca
// 4Reven 5Boss1 6Dog 7FH-SF 8Boss3 9Boss2 (из per-enemy draw-fn: FH walk0/fire7/stagger1/death4 и т.д.).
// ⚠ ВСЕ выверено: draw-fn каждого врага (state→класс→anim) + РЕНДЕР кадров (декодер как ztextractor: nfr==0→1 кадр,
// поэтому «пустых» анимов НЕТ — это были 1-кадровые позы hit/death). hit=stagger(HP≥0), death=stagger(HP<0).
// ⭐ death = CORPSE-anim (труп, actor+0xe corpse-draw — НЕ death-стаггер-поза state2!) — VERIFIED 2026-07-09 по
// objdef@0xAB0C + corpse-draw d0. Правки: Hydaca 8→9, Boss1 4→6, Boss3 6→9, Boss2 2→4; hit Hydaca −1→1.
static const int ENEMY_ANIM_IDX[10][4] = {   // {walk, fire, hit, death(=corpse)}
    {0, 3,  7, 11}, // Sgt:   fire=a3 hit=a7 corpse=a11(комок 32×32, corpse-draw 1b68c)
    {0, 7,  1,  4}, // FH:    fire=a7 hit=a1 corpse=a4(5к, мерцает LUT@186a8 — дёргается; corpse-draw 18660)
    {0, 2,  3,  6}, // Imp:   strike=a2 hit=a3 corpse=a6(5к, кадр0/1 по $35; corpse-draw 18ac8)
    {0,-1,  1,  9}, // Hydaca:пол=a0/потолок=a5(walkB)/climb=a1; hit=a1; corpse=a9(7к, RNG-мерцание=ножки; 15618)
    {0, 1,  3,  7}, // Reven: fire=a1 hit=a3 corpse=a7(комок 32×32; corpse-draw 1ae9e)
    {0, 1,  3,  6}, // Boss1: fire=a1 hit=a3 corpse=a6(64×32 лёжа; corpse-draw 19430 — было a4=death-стаггер)
    {0, 1,  3,  6}, // Dog:   lunge=a1 hit=a3 corpse=a6(64×32 лёжа; corpse-draw 1994c)
    {0, 5,  1,  4}, // FH-SF: fire=a5 hit=a1 corpse=a4(5к; corpse-draw 19fca)
    {0, 2,  5,  9}, // Boss3: fire=a2 (контакт-УДАР «руки вверх», ZT state2/3 draw 1a786; a1=дальний state1) hit=a5 corpse=a9
    {0, 5,  1,  4}  // Boss2: fire=a5 hit=a1 corpse=a4(64×32; corpse-draw 18f50 — было a2=death-стаггер)
};
// ⭐2-я БОЕВАЯ поза по слоту (−1 = нет; 2026-07-15). ROM даёт врагу ДВЕ боевые анимации (fire=основная в ENEMY_ANIM_IDX):
//   Sgt a4=бросок гранаты (1b600 state5), Boss1 a2=выстрел-вспышка (19402 $35∈4/5/6), Dog a2=прыжок-укус (198c4 $35<6||dist<1кл),
//   FH-SF a11=замах гранаты (19f5c state5), Boss3 a4=дальняя атака (01a7d0 state1; fire=a2 ближний), Boss2 a6=удар (018f44; fire=a5 aim).
//   ⭐Imp a1 = ЗАМАХ (ROM draw 18aa0: окно атаки $35 10..7 и 3..0 → кадр 1 «руки подняты», 4..6 → кадр 2 удар).
static const int ENEMY_ANIM_IDX2[10] = { 4, -1, 1, -1, -1, 2, 2, 11, 4, 6 };
// 2-я ВАРИАЦИЯ ходьбы по слоту (−1 = нет). FH (slot1) имеет 2-й полный 6-напр walk-набор (anim9) в том же банке.
// Hydaca(3): anim5 = ПОТОЛОК-краул (variant драйвится z: на потолке→1; пол=anim0).
static const int ENEMY_VARIANT_WALK[10] = { -1, -1, -1, 5, -1, -1, -1, -1, -1, -1 };
// Спрайт лазанья по СТЕНЕ (вертикальный) по слоту: только Hydaca(3) = anim1 (32×64).
static const int ENEMY_CLIMB_ANIM[10] = { -1, -1, -1, 1, -1, -1, -1, -1, -1, -1 };
// АЛЬТ-БАНК ГРАФИКИ (ПОЛНОСТЬЮ другой набор спрайтов). FH(1): 0x1C258A — мускулистый коммандо
// (ztextractor ZT_CELLTYPE_ENEMY[0x2A]=(...,0x16EC58,0x1C258A)). «Две вариации FH при одном cell id» = разные банки.
static const unsigned ENEMY_ALT_GFX[10] = { 0, 0x1C258A, 0, 0, 0, 0, 0, 0, 0, 0 };

void SpriteClut::load(const Rom& rom, size_t base) {
    if (base + 12 * 0x100 > rom.size()) return;
    for (int b = 0; b < 12; ++b)
        for (int i = 0; i < 256; ++i) band[b][i] = rom.u8(base + (size_t)b * 0x100 + i);
    ok = true;
}

// Декод одного кадра: tileBase=(a1+2)+u16(a1+2); animCount=u16(a1); animBlock=(a1+2)+u16(a1+4+anim*2);
// dirCount=u16(animBlock); dirBlock=animBlock+u16(animBlock+2+dir*2); frameCount=u16(dirBlock)(0→1);
// frame=dirBlock+2+f*0x26: +4 W(тайлов) +5 H, +0x06 attr[W*H stride4 bit0=hflip], +0x16 tileIdx[stride4].
static EnemySprite decodeEnemySprite(const Rom& rom, size_t a1, const Palette& pal, int anim, int dir, int frame) {
    EnemySprite s;
    if (!a1 || a1 + 6 >= rom.size()) return s;
    size_t base = a1 + 2;
    size_t tileBase = base + rom.u16(a1 + 2);
    int animCount = rom.u16(a1); if (animCount < 1 || animCount > 32) return s;
    if (anim >= animCount) anim = 0;
    size_t animBlock = base + rom.u16(a1 + 4 + (size_t)anim * 2);
    int dirCount = rom.u16(animBlock); if (dirCount < 1 || dirCount > 32) return s;
    if (dir >= dirCount) dir = 0;
    size_t dirBlock = animBlock + rom.u16(animBlock + 2 + (size_t)dir * 2);
    int frameCount = rom.u16(dirBlock); if (frameCount == 0) frameCount = 1; if (frameCount > 64) return s;
    if (frame >= frameCount) frame = 0;
    size_t fr = dirBlock + 2 + (size_t)frame * 0x26;
    int W = rom.u8(fr + 4), H = rom.u8(fr + 5);
    if (W < 1 || W > 8 || H < 1 || H > 8) return s;
    s.drawW = rom.u8(fr + 2); s.drawH = rom.u8(fr + 3);    // on-screen scale: W=drawW·scale>>5, H=drawH·scale>>4
    if (s.drawW < 1) s.drawW = W * 6; if (s.drawH < 1) s.drawH = H * 6;
    s.w = W * 32; s.h = H * 32; s.argb.assign((size_t)s.w * s.h, 0u);
    s.idx.assign((size_t)s.w * s.h, 0);            // сырые пал.индексы (для CLUT-шейда при рендере)
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c) {
            uint8_t idx  = rom.u8(fr + 0x16 + (size_t)r * 4 + c);
            bool   flip  = rom.u8(fr + 0x06 + (size_t)r * 4 + c) & 1;
            size_t ta = tileBase + (size_t)idx * 512;
            if (ta + 512 > rom.size()) continue;
            for (int col = 0; col < 16; ++col)
                for (int row = 0; row < 32; ++row) {
                    uint8_t b = rom.u8(ta + (size_t)col * 32 + row);
                    int x0 = col * 2, x1 = col * 2 + 1; uint8_t p0 = b >> 4, p1 = b & 0x0F;
                    if (flip) { x0 = 31 - x0; x1 = 31 - x1; }
                    int py = r * 32 + row;
                    if (p0) { s.argb[(size_t)py * s.w + c * 32 + x0] = pal.c[p0]; s.idx[(size_t)py * s.w + c * 32 + x0] = p0; }
                    if (p1) { s.argb[(size_t)py * s.w + c * 32 + x1] = pal.c[p1]; s.idx[(size_t)py * s.w + c * 32 + x1] = p1; }
                }
        }
    s.ok = true; return s;
}

void decodeEnemySprites(const Rom& rom, const Palette& pal, const size_t* gfxOverride, size_t altFH, size_t clutBase) {
    g_spriteClut().load(rom, clutBase);             // CLUT спрайтов (шейд при рендере)
    static const size_t GFX_ZT[10] = { 0x1B7B38, 0x16EC58, 0x1A7828, 0x1ACC72, 0x18D078,
                                       0x183B94, 0x1784F2, 0x17BA80, 0x193F00, 0x19D50C };
    const size_t* GFX = gfxOverride ? gfxOverride : GFX_ZT;
    auto decAnim = [&](size_t a1, int anim, int dir, std::vector<EnemySprite>& out) {
        if (anim < 0) return;
        int animCount = rom.u16(a1); if (anim >= animCount) return;
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)anim * 2);
        int dirc = rom.u16(animBlock); if (dirc < 1) dirc = 1;
        if (dir >= dirc) return;
        size_t dirBlock = animBlock + rom.u16(animBlock + 2 + (size_t)dir * 2);
        int fc = rom.u16(dirBlock); if (fc == 0) fc = 1; if (fc > 12) fc = 12;
        for (int f = 0; f < fc; ++f) { EnemySprite s = decodeEnemySprite(rom, a1, pal, anim, dir, f);
            if (s.ok) out.push_back(std::move(s)); }
    };
    for (int i = 0; i < 10; ++i) {
        EnemyAnimSet& A = g_enemyAnim[i]; A = EnemyAnimSet{};
        size_t a1 = GFX[i];
        int wA = ENEMY_ANIM_IDX[i][0], fA = ENEMY_ANIM_IDX[i][1], hA = ENEMY_ANIM_IDX[i][2], dA = ENEMY_ANIM_IDX[i][3];
        // ХОДЬБА = anim wA по ВСЕМ направлениям (повороты). dir0=фронт.
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)wA * 2);
        int dirs = rom.u16(animBlock); if (dirs < 1) dirs = 1; if (dirs > 6) dirs = 6;
        A.walkDirs = dirs;
        for (int d = 0; d < dirs; ++d) decAnim(a1, wA, d, A.walk[d]);
        if (A.walk[0].empty()) decAnim(a1, 0, 0, A.walk[0]);           // фолбэк
        decAnim(a1, fA, 0, A.fire);                                    // стрельба/удар
        decAnim(a1, hA, 0, A.hit);                                     // стаггер
        // ⭐DIRECTIONAL огонь/стаггер (ROM 1ba04 разруливает виды ГЕОМЕТРИЧЕСКИ у любого кадра — не только ходьбы)
        auto decDirs = [&](int anim, std::vector<EnemySprite>* out, int& cnt) {
            if (anim < 0 || anim >= rom.u16(a1)) return;
            size_t blk = (a1 + 2) + rom.u16(a1 + 4 + (size_t)anim * 2);
            int dn = rom.u16(blk); if (dn < 1) dn = 1; if (dn > 6) dn = 6;
            for (int d = 0; d < dn; ++d) decAnim(a1, anim, d, out[d]);
            if (!out[0].empty()) cnt = dn;
        };
        decDirs(fA, A.fireD, A.fireDirs);
        decDirs(hA, A.hitD, A.hitDirs);
        decAnim(a1, dA, 0, A.death);                                   // смерть
        if (ENEMY_ANIM_IDX2[i] >= 0) decAnim(a1, ENEMY_ANIM_IDX2[i], 0, A.fire2);   // 2-я боевая поза (прыжок/выстрел/дальний/удар/бросок)
        // 2-я вариация ходьбы (FH anim9): per-actor выбор по variant.
        int vA = ENEMY_VARIANT_WALK[i];
        if (vA >= 0 && vA < rom.u16(a1)) {
            size_t vBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)vA * 2);
            int vdirs = rom.u16(vBlock); if (vdirs < 1) vdirs = 1; if (vdirs > 6) vdirs = 6;
            for (int d = 0; d < vdirs; ++d) decAnim(a1, vA, d, A.walkB[d]);
            if (!A.walkB[0].empty()) { A.walkBDirs = vdirs; A.hasVariant = true; }
        }
        decAnim(a1, ENEMY_CLIMB_ANIM[i], 0, A.climb);                  // Hydaca: вертикальный спрайт лазанья
        // DIRECTIONAL climb (Hydaca): 6 вертик. поз лазанья по ракурсу — ZT climb states 3-A → a1/a2/a3/a4/a6/a7.
        if (i == 3) {                                                  // только Hydaca
            static const int CLIMB6[6] = { 1, 2, 3, 4, 6, 7 };
            for (int d = 0; d < 6; ++d) decAnim(a1, CLIMB6[d], 0, A.climbDir[d]);
            if (!A.climbDir[0].empty()) A.climbDirs = 6;
            decAnim(a1, 8,  0, A.fallUp);                               // a8  = падение вверх/срыв (ZT draw state2 $2e>0)
            decAnim(a1, 10, 0, A.fallDead);                             // a10 = падение мёртвой (ZT draw state2 HP<0)
        }
        if (i == 0) decAnim(a1, 6, 0, A.morph);                         // Sgt: a6 = МОРФ (ZT 1b628, 5 ступеней по $35)
        if (i == 4) {                                                   // Revenant: СМЕРТЕЛЬНЫЙ стаггер = падение (ZT draw 1adfe, HP<0)
            decAnim(a1, 8, 0, A.revFall[0]);                            //   $35 10..9 → a8 (подбит)
            decAnim(a1, 4, 0, A.revFall[1]);                            //   $35 8..7  → a4
            decAnim(a1, 6, 0, A.revFall[2]);                            //   $35 6..5  → a6
            decAnim(a1, 5, 0, A.revFall[3]);                            //   $35 4..0  → a5 (лёг; после — звук 0x27 + кадр 7)
        }
        if (i == 8) {                                                   // Boss3: притворство мёртвым (ZT 1a752/1a7dc)
            decAnim(a1, 10, 0, A.pretendLie);                           // a10 = лежит смирно ($35<5)
            decAnim(a1, 7,  0, A.pretendJerkA);                         // a7  = дёрг (15/16)
            decAnim(a1, 8,  0, A.pretendJerkB);                         // a8  = дёрг-альт (1/16)
        }
        A.ok = !A.walk[0].empty();
        g_enemyWalk[i] = A.walk[0];                                    // совместимость
    }
    // АЛЬТ-БАНКИ: полный второй набор спрайтов (другая модель). Сейчас FH(1)=0x1C258A (коммандо).
    for (int i = 0; i < 10; ++i) {
        EnemyAnimSet& A = g_enemyAnimVar2[i]; A = EnemyAnimSet{};
        size_t a1 = (i == 1) ? altFH : ENEMY_ALT_GFX[i] && !gfxOverride ? ENEMY_ALT_GFX[i] : 0;  // альт-банк: FH per-build, прочие ZT-only
        if (!a1) continue;
        int wA = ENEMY_ANIM_IDX[i][0], fA = ENEMY_ANIM_IDX[i][1], hA = ENEMY_ANIM_IDX[i][2], dA = ENEMY_ANIM_IDX[i][3];
        size_t animBlock = (a1 + 2) + rom.u16(a1 + 4 + (size_t)wA * 2);
        int dirs = rom.u16(animBlock); if (dirs < 1) dirs = 1; if (dirs > 6) dirs = 6;
        A.walkDirs = dirs;
        for (int d = 0; d < dirs; ++d) decAnim(a1, wA, d, A.walk[d]);
        if (A.walk[0].empty()) decAnim(a1, 0, 0, A.walk[0]);
        decAnim(a1, fA, 0, A.fire); decAnim(a1, hA, 0, A.hit); decAnim(a1, dA, 0, A.death);
        A.ok = !A.walk[0].empty();
    }
    // ⭐BURNT REMAINS (ZT 0x1b8d8 спрайт-банк 0x1cb96a, draw 1b952): 1 anim / 1 dir / 4 кадра. Труп спалённого огнём врага.
    // Только ZT-раскладка (адрес ZTU-банка не вскрыт — при override пропускаем, потребители держат пустой список).
    g_burntRemains.clear();
    if (!gfxOverride)
        for (int f = 0; f < 4; ++f) { EnemySprite s = decodeEnemySprite(rom, 0x1cb96a, pal, 0, 0, f); if (s.ok) g_burntRemains.push_back(std::move(s)); }
    // КАМЕРА-ТРЕВОГА (0x26): draw 15d84 грузит a1=0x110FBE/0x1111BE в eda0, НО эти адреса = нулевые провалы между
    // блоками графики (тайлы на 0x110F00/0x111000). eda0 читает a1 не как обычный actor-дескриптор → реальный спрайт
    // требует разбора блиттера eda0/формата (TODO, отд. RE). Пока камера рисуется плейсхолдер-декором (pushCameraBillboards).
}
