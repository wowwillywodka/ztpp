// ztpp — src/weapons.cpp: холодные не-шаблонные функции оружия/инвентаря (стрельба/подбор/HUD).
// Шаблоны отрисовки в руках (template<class Put>) и мелкие хелперы — в weapons.hpp.
#include "weapons.hpp"
#include "sniper_overlay.hpp"   // фоновый снайпер: хвост конуса 167b0 (убийство по X-центру)

// ⭐УБИЙСТВО ФОНОВОГО СНАЙПЕРА (хвост 167b0 @16862): исполняется при КАЖДОМ вызове конус-наведения
// (кулак/граната/огнемёт/hitscan — ROM: хвост внутри 167b0). Условия: этаж 0 (-$6fd8==0), игрок в
// ct 0x28 (любой pitch) или 0x27 с pitch≥−5 (из глубокого приседа НЕ пострелять — укрытие в обе
// стороны), снайпер активен, |X_прицела(центр 0x120) − X_снайпера| ≤ 3 → фаза 0x20 → 0x24 → жизнь−1.
// Звука убийства в ROM НЕТ (только link-пакет 29ba).
static void snipTryAimKill(const Level& lvl, const Camera& cam) {
    if (!faSnipers() || cam.floor != 0) return;                      // 1685a
    int cx = (int)cam.px, cy = (int)cam.py;
    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return;
    uint8_t ct = lvl.cellType(cam.floor, cx, cy);
    double camH = player().crouchY + player().knockPitch;            // -71e6 (полный питч)
    if (!(ct == 0x28 || (ct == 0x27 && camH >= -5.0))) return;       // 16878/1687e
    snip::aimKill();
}

int rcTryPickup(Inventory& inv, const Level& lvl, int floor, double px, double py) {
    int x = (int)px, y = (int)py;
    if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) return -1;
    uint8_t ct = lvl.cellType(floor, x, y);
    int k = pickKey(floor, x, y);
    if (pickedSet().count(k)) return -1;             // уже подобрано
    if (ct == 0x25) {                                // МЕДПАК (+HP, не инвентарь)
        if (player().hp >= player().maxHp) return -1;   // полное HP → не подбирать (ZT 0x11b7c)
        pickedSet().insert(k); healPlayer(20); return PICK_MEDKIT;
    }
    int idx = pickupItemIdx(ct);
    if (idx < 1 || idx >= 15) return -1;
    // ⭐ЕДИНАЯ логика (applyPickup): кап-блок (полный → НЕ брать), руки-vs-слот, мигание. false → клетку НЕ гасим (остаётся на полу).
    if (!applyPickup(inv, idx)) return -1;           // инвентарь полон / кап-блок (ammo>=cap) → предмет остаётся в мире
    pickedSet().insert(k);                           // взято → гасим клетку
    return idx;
}

void fireSpawn(const Inventory& inv, const Level& lvl, const Camera& cam) {
    int id = inv.current;
    int kind = heldDisplayKind(id);
    if (kind == 0) {                                            // ⭐КУЛАЧНЫЙ БОЙ (ZT 11f3c, verified): мили-КОНУС (d1=0),
        // БЛИЗКАЯ дальность (scale>0xa0 ≈ <0.4кл; БОЕЦ3 >0x64 ≈ <0.64кл — длиннее рука). Урон X→(0x400−X): база 0x300
        // (dmg 256); БОЕЦ3 X=0xc8 (dmg 824, силач, ≥0x338=с кровью); БОЕЦ4 X=0x1f4 (dmg 524). Кулаки НАНОСЯТ урон
        // (заблуждение «кулаки безвредны» — неверно; урон в анимации на кадре контакта). Руки чередуются (punchSide).
        int fg = playerFighter();
        snd::playSfx(0x1b);                                     // ЗАМАХ на КАЖДЫЙ удар (ZT 12bac move #$1b,d0→d760); лёгкий «вжух»
        int reachScale = (fg == 3) ? 100 : 160;                 // порог ближней дальности (0x64 / 0xa0)
        int ti = coneTargetEnemy(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY, 0, reachScale);
        snipTryAimKill(lvl, cam);        // хвост 167b0 — исполняется и на кулачном конусе (ROM)
        if (ti >= 0) {
            int X = (fg == 3) ? 0xc8 : (fg == 4) ? 0x1f4 : 0x300;   // X по бойцу (12118/12124/12130)
            int raw = 0x400 - X;                                     // урон = 0x400 − X (187c8)
            Actor& a = actors()[ti];
            if (a.think == AT_CORPSE) corpseHit(a, cam.px, cam.py, raw);   // пнуть/ударить ТРУП (из приседа) → лёгкий отлёт
            else { int dmg = raw / 100 < 1 ? 1 : raw / 100; hitEnemy(a, dmg, cam.px, cam.py, raw); }
            snd::playSfx(inv.punchVariant >= 2 ? 0x1a : 0x17);      // КОНТАКТ по врагу: кик-вниз(вар2)=0x1a / удар-нейтр/вверх=0x17 (ZT 12146: var<2→0x1a; var нейтр=3/вверх=5/вниз=1)
        } else {                                                     // промах: удар в стену вплотную → 0x1c; в воздух → только замах 0x1b (ZT 12178 beq)
            int cx = (int)(cam.px + cam.dirX * 0.5), cy = (int)(cam.py + cam.dirY * 0.5);
            if (cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H &&
                cellBlockedAt(lvl.cellType(cam.floor, cx, cy), 0.5, 0.5)) snd::playSfx(0x1c);
        }
        return;
    }
    double x = cam.px + cam.dirX * 0.4, y = cam.py + cam.dirY * 0.4;
    if (id == 2)  {                                             // МИНА — ПРЯМО ПЕРЕД игроком (ZT 0x12bfe: forward/16 ×8 ≈ ½ кл)
        double mx = cam.px, my = cam.py;
        for (int i = 0; i < 8; ++i) {                           // мелкий шаг (½/16 ≈ 0.03 кл) до стены — итог ~0.5 кл
            double nx = mx + cam.dirX * 0.0625, ny = my + cam.dirY * 0.0625;
            if (cellBlocks(lvl.cellType(cam.floor, (int)nx, (int)ny))) break;
            mx = nx; my = ny;
        }
        spawnMine(mx, my, cam.floor); return;
    }
    if (id == 13) {                                            // ⭐ОГНЕМЁТ: конус-наведение (ZT 167b0 в 12946, d1=8) → короткий
        int ti = coneTargetEnemy(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY, 8);  //   lifetime к врагу → AoE-импакт достаёт.
        snipTryAimKill(lvl, cam);                                                         // хвост 167b0 (конус 12946)
        double td = (ti >= 0) ? gameDist(actors()[ti].x - cam.px, actors()[ti].y - cam.py) : -1.0;
        spawnFlameP(x, y, cam.floor, cam.dirX, cam.dirY, td); return;                     // ~1 частица/кадр (ZT ~0.75; было 2 = слишком плотно)
    }
    if (id == 4)  {                                            // ОГНЕТУШИТЕЛЬ (ZT 0x12cfe): ОДИН спрайт пены (БЕЗ разброса), падает вниз, тушит огонь
        double mx = cam.px + cam.dirX * 0.35, my = cam.py + cam.dirY * 0.35;
        spawnFoam(mx, my, cam.floor, cam.dirX, cam.dirY);
        return;
    }
    if (id == 7)  {                                            // ГРАНАТА (ZT 12e7c→11e64): автонаведение 167b0 →
        // фитиль = dist/64+8 (11e3c), vel к упреждённой цели (цель+vel×4) — ложится на цель; без цели фитиль 0x32=50.
        double gx = cam.px + cam.dirX * 0.3, gy = cam.py + cam.dirY * 0.3;
        int gt = coneTargetEnemy(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY, 8);
        snipTryAimKill(lvl, cam);        // хвост 167b0 (конус 11e64)
        if (gt >= 0 && actors()[gt].think == AT_ENEMY) {
            Actor& t = actors()[gt];
            double lx = t.x + t.vx * 4.0, ly = t.y + t.vy * 4.0;   // упреждение цель+vel×4 (11e2c)
            double dd = gameDist(lx - cam.px, ly - cam.py);
            int fuse = (int)(dd * 4.0) + 8;                         // dist/64 units = dd·4 тиков
            if (playerFighter() == 1) { fuse /= 2; if (fuse < 1) fuse = 1; }   // боец1: vel×2 / фитиль÷2 (11ea4)
            spawnGrenadeAimed(gx, gy, cam.floor, (lx - gx) / fuse, (ly - gy) / fuse, fuse);
        } else spawnGrenade(gx, gy, cam.floor, cam.dirX, cam.dirY, 0,
                            playerFighter() == 1 ? 2.0 : 1.0);      // без цели (БОЕЦ1: дальность ×2, ZT 12e7c)
        return; }
    if (id == 11) { spawnBullet(x, y, cam.floor, cam.dirX, cam.dirY, 0.5, A_EXPL_TILE, 200); return; }  // РАКЕТА vel=0.5кл/кадр (ZT dir/2)
    // ⭐HIT-SCAN (handgun/laser/shotgun/pulse): КОНУС-АВТОНАВЕДЕНИЕ (ZT 167b0) — ближайший враг в конусе оружия;
    // урон = дист-кривая (playerWeaponRawDamage, с перками бойца). Промах/вне дальности → искра у стены / прострел трупа.
    // minScale = depth-порог стены в прицеле (ZT d0=-$1e5a): нельзя навестись на врага ДАЛЬШЕ стены перед прицелом.
    int wallScale = aimWallScale(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY);
    int ti = coneTargetEnemy(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY, coneWidth(id), wallScale);
    snipTryAimKill(lvl, cam);            // хвост 167b0 — и при попадании в актёра тоже (ROM: хвост безусловен)
    if (ti >= 0) {
        Actor& a = actors()[ti];
        double dd = gameDist(a.x - cam.px, a.y - cam.py);
        int raw = playerWeaponRawDamage(id, dd);
        // ⭐ВЫСТРЕЛ ВО ВЗРЫВЧАТКУ (ROM receive 13ad2: d0<0x200 → детонация 13dcc). raw = 0x400−d0 → условие raw>0x200.
        // Мина отфильтрована стойкой ещё в конусе (flags 0x8 → только присед); граната/ракета — из любой стойки.
        if (a.think == AT_MINE || a.think == AT_BULLET || a.think == AT_GRENADE) {
            if (raw > 0x200) { detonateActor(lvl, a, cam); return; }
        } else if (raw > 0) {
            if (a.think == AT_CORPSE) { corpseHit(a, cam.px, cam.py, raw); return; }   // ТРУП (в конусе из приседа) → отлёт ПО ОРУЖИЮ
            int dmg = raw / 100 < 1 ? 1 : raw / 100; hitEnemy(a, dmg, cam.px, cam.py, raw); return;
        }
    }
    // ⭐КАМЕРА-ТРЕВОГА (0x26): сбивается ТОЛЬКО В ПРЫЖКЕ (целясь ВВЕРХ — ROM стойка-маска 0x80 vs флаг камеры 0x84).
    if (playerStanceMask() == 0x80 && traceCameraHit(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY)) return;
    traceMiss(lvl, cam.floor, cam.px, cam.py, cam.dirX, cam.dirY);   // нет цели в конусе / вне дальности → импакт у стены (сквозь труп)
}

void drawInventoryHud(uint32_t* frame, int FW, int FH, const GameData& gd, const Inventory& inv) {
    std::vector<int> r = inv.ring();                      // кольцо: значения слотов (−1 = пусто/кулаки)
    int n = (int)r.size();
    if (gd.hudIcons.empty() || n == 0) return;
    static const int SLOT_X[5] = {40, 88, 144, 200, 248};
    const int SLOT_Y = 3;
    auto px = [&](int x, int y, uint32_t c) { if (x >= 0 && x < FW && y >= 0 && y < FH) frame[(size_t)y * FW + x] = c; };
    uint8_t ip[32 * 32];
    for (int k = 0; k < 5; ++k) {
        int j = inv.sel + (k - 2);                       // центр (k=2) = текущий
        if (n >= 5) j = ((j % n) + n) % n;               // карусель-обмотка (окно 5 вокруг текущего)
        else if (j < 0 || j >= n) continue;              // <5 слотов: пустые края
        int id = r[j];
        if (id < 0) continue;                            // пустой слот / кулаки — иконки нет
        if (!inv.iconVisible(id)) continue;              // ⭐МИГАНИЕ после подбора (ZT 0108d6): иконка скрыта на blink T=3/1
        int icon = gd.iconForId(id);
        if (icon < 0) continue;
        gd.decodeIcon(icon, ip);
        int bx = SLOT_X[k];
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x) { uint8_t i = ip[y * 32 + x]; if (i) px(bx + x, SLOT_Y + y, gd.heldPal.c[i]); }
        // боезапас — маленький номер ZT-шрифтом в НИЗ-ЛЕВО углу (FUN_1DF54). Оружие-счётчик (кап 99) = count;
        // расходники (кап 10 @0x11240: сканер/жилет/огнетуш/костюм/фонарь/ночник/огнемёт) = ПРОЦЕНТ 0..100 (ammo·100/cap).
        if (id >= 1 && id < 15) {
            bool pct = ITEMS[id].ammoCap <= 10;                       // расходник → % / оружие → count
            int cap = ITEMS[id].ammoCap > 0 ? ITEMS[id].ammoCap : 1;
            int a = pct ? (inv.ammo[id] * 100 / cap) : inv.ammo[id];
            // расходник: ПОЛНЫЙ заряд = 100% (юзер: у процентных айтемов есть 100%). len=3 → плашка/знак % ниже
            // берут ширину из len, поэтому «100%» размещается корректно. Оружие-счётчик держим в 2 цифрах (кап 99).
            if (pct) { if (a > 100) a = 100; } else if (a > 99) a = 99;
            if (a < 0) a = 0;
            char b[8]; int len = std::snprintf(b, sizeof b, "%d", a);
            int glyphs = len + (pct ? 1 : 0);                          // +1 клетка под знак '%'
            int dx = bx + 1, dy = SLOT_Y + 25;                         // низ-лев. угол иконки
            for (int yy = -1; yy < 7; ++yy) for (int xx = -1; xx < glyphs * 4; ++xx) px(dx + xx, dy + yy, 0xFF000000u);  // плашка-тайл
            uint32_t dcol = 0xFFFFFF00u;                                // цвет цифр (реальный берём из ZT-шрифта ниже)
            for (int c = 0; c < len; ++c) {
                int d = b[c] - '0';
                for (int ry = 0; ry < 6; ++ry) for (int rx = 0; rx < 4; ++rx) {
                    uint8_t pi = gd.digitPx(d, rx, ry);
                    if (pi) { dcol = gd.heldPal.c[pi]; px(dx + c * 4 + rx, dy + ry, dcol); }
                }
            }
            if (pct) {                                                 // знак '%' у расходников (порт-подача 0..100%)
                static const uint8_t PCT[6] = {0x8, 0x9, 0x2, 0x4, 0x9, 0x1};   // 4x6, col0=bit3 .. col3=bit0
                int gx = dx + len * 4;
                for (int ry = 0; ry < 6; ++ry) for (int rx = 0; rx < 4; ++rx)
                    if (PCT[ry] & (0x8 >> rx)) px(gx + rx, dy + ry, dcol);
            }
        }
    }
    // подсветка ТЕКУЩЕГО (центральная панель): тонкая рамка
    int rx = SLOT_X[2] - 1, ry = SLOT_Y - 1, rw = 34, rh = 34;
    for (int x = 0; x < rw; ++x) { px(rx + x, ry, 0xFF80FF80u); px(rx + x, ry + rh - 1, 0xFF80FF80u); }
    for (int y = 0; y < rh; ++y) { px(rx, ry + y, 0xFF80FF80u); px(rx + rw - 1, ry + y, 0xFF80FF80u); }
}
