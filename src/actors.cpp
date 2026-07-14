// ztpp — src/actors.cpp: гигантский per-frame AI/update-цикл updateActors (вынесен из actors.hpp).
// Не-шаблон, не per-pixel (раз в кадр) → компилируется один раз, не в каждой TU. Прочая логика — в actors.hpp.
#include "actors.hpp"

void updateActors(const Level& lvl, const Camera& cam) {
    auto& v = actors();
    openDoorsAtEnemies(lvl, cam.floor);   // ZT b35c: дверь держится ОТКРЫТОЙ, пока в её клетке актёр (bit4)
    // скорость игрока (для УПРЕЖДЕНИЯ точки прицела врага, как ZT добавляет target.vx/vy): дельта позы за кадр
    static double s_ppx = cam.px, s_ppy = cam.py;
    double pvx = cam.px - s_ppx, pvy = cam.py - s_ppy; s_ppx = cam.px; s_ppy = cam.py;
    { PlayerState& p = player(); if (p.iframe > 0) --p.iframe; if (p.flash > 0) --p.flash;
      double tgt = (p.knockTimer > 0) ? -16.0 : 0.0;             // НОКДАУН: питч в пол (ZT -0x71e4=-0x10), потом восстанавливается
      if (p.knockTimer > 0) --p.knockTimer;
      if (p.knockPitch < tgt) { p.knockPitch += 4; if (p.knockPitch > tgt) p.knockPitch = tgt; }   // ±4/кадр (ZT)
      else if (p.knockPitch > tgt) { p.knockPitch -= 4; if (p.knockPitch < tgt) p.knockPitch = tgt; } }
    for (auto& a : v) {
        if (!a.active) continue;
        switch (a.think) {
            case AT_BULLET: {                            // летит → взрыв о стену / по ВРАГУ / по истечении
                double nx = a.x + a.vx, ny = a.y + a.vy;
                int cx = (int)nx, cy = (int)ny;
                bool hit = (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) ||
                           cellBlockedAt(lvl.cellType(a.floor, cx, cy), nx - cx, ny - cy);  // диагональ-полуплоскость
                // РАКЕТА В ВРАГА (ZT снаряд 15f6e: актёр в ≤0x40≈0.25кл → детонация) — иначе пролетала насквозь.
                // НЕ снапим в позицию врага (иначе спрайт взрыва ложится ПОВЕРХ врага) — взрыв на месте ракеты, радиус достанет.
                if (!hit) for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                                gameDist(e.x - nx, e.y - ny) < 0.5) { hit = true; break; }
                if (hit) {                                // взрыв ЧУТЬ ПЕРЕД стеной (иначе z-режется стеной)
                    double sp = std::hypot(a.vx, a.vy);
                    if (sp > 0 && (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                                   cellBlockedAt(lvl.cellType(a.floor, cx, cy), nx - cx, ny - cy)))
                        { a.x -= a.vx / sp * 0.4; a.y -= a.vy / sp * 0.4; }
                    a.active = false; explodeAt(lvl, a.x, a.y, a.floor, cam);   // взрыв ракеты: урон врагам/игроку (снять ДО — анти-цепь-рекурсия)
                } else { a.x = nx; a.y = ny; if (--a.timer <= 0) { a.active = false; explodeAt(lvl, a.x, a.y, a.floor, cam); } }
                break;
            }
            case AT_GRENADE: {                           // ФИЗИКА (0x13adc): дуга+гравитация+отскок, фитиль → взрыв
                bool nearTgt = false;                     // взрыв по условию: цель рядом
                if (a.state == 1) {                       // граната ВРАГА → детонирует у ИГРОКА
                    nearTgt = (a.floor == cam.floor && std::hypot(cam.px - a.x, cam.py - a.y) < 0.7);
                } else {                                  // граната ИГРОКА → детонирует у ВРАГА
                    for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                          std::hypot(e.x - a.x, e.y - a.y) < 0.6) { nearTgt = true; break; }
                }
                if (--a.timer <= 0 || nearTgt) { explodeAt(lvl, a.x, a.y, a.floor, cam, a.z); a.active = false; break; }  // взрыв на ВЫСОТЕ гранаты (z), не у глаз
                double nx = a.x + a.vx, ny = a.y + a.vy;   // горизонталь + отскок от стен (reflect+damp, как neg/asr)
                bool bounced = false;
                if (cellBlocks(lvl.cellType(a.floor, (int)nx, (int)a.y))) { a.vx = -a.vx * 0.5; nx = a.x; bounced = true; }
                if (cellBlocks(lvl.cellType(a.floor, (int)a.x, (int)ny))) { a.vy = -a.vy * 0.5; ny = a.y; bounced = true; }
                if (nx >= 0 && ny >= 0 && nx < Level::W && ny < Level::H) { a.x = nx; a.y = ny; }
                a.vz -= 0.0035; a.z += a.vz;               // гравитация ($2e -= ; $24 += )
                if (a.z <= 0) { a.z = 0; if (a.vz < -0.02) { a.vz = -a.vz * 0.4; a.vx *= 0.7; a.vy *= 0.7; bounced = true; } else a.vz = 0; }  // отскок от пола
                if (bounced && a.floor == cam.floor) snd::playSfx(0x6c);   // РИКОШЕТ гранаты о стену = 0x6c (ZT think 0x13b0a: play 0x6c → neg/asr velocity); тот же «клац», что установка мины
                break;
            }
            case AT_FLAME: {                             // ⭐СТРУЯ огнемёта (ZT think 0x142c6 / спавн 0x1299a): частица
                // ЛЕТИТ с гравитацией (zVel=−2, +1/кадр → всплыв→опад), в ПОЛЁТЕ НЕ жжёт; при ИМПАКТЕ (стена / пол
                // Z≤0 / таймаут $34, ZT $14300) → урон в ≤1кл ОДИН раз + превращается в НАЗЕМНЫЙ ОГОНЬ (ZT: draw
                // 0x14272, Z на полу), который ЛИНГЕРИТ ~20 кадров и жжёт горючих (как AT_FIRE). Игрока своим огнём не жжёт.
                a.z += a.vz; a.vz -= 0.006;                                           // всплыв (z-vel вверх) → опад (гравитация +1/кадр)
                double nx = a.x + a.vx, ny = a.y + a.vy;
                int cx = (int)nx, cy = (int)ny;
                bool land = (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) ||
                            cellBlocks(lvl.cellType(a.floor, cx, cy)) || a.z <= 0.0 || --a.timer <= 0;  // стена / пол / таймаут
                if (land) {
                    for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&      // ROM $14300: урон ОДИН раз в ≤1кл
                                          std::hypot(e.x - a.x, e.y - a.y) < 1.0) { e.burned = true; hitEnemy(e, 2, a.x, a.y); }
                    int fx0 = (int)a.x, fy0 = (int)a.y;
                    if (fx0 >= 0 && fy0 >= 0 && fx0 < Level::W && fy0 < Level::H && !cellBlocks(lvl.cellType(a.floor, fx0, fy0)))
                        spawnFire(a.x, a.y, a.floor, A_FIRE_LIFE, true);              // НАЗЕМНЫЙ ОГОНЬ (лингерит + жжёт; playerSafe)
                    a.active = false; break;
                }
                a.x = nx; a.y = ny;                                                  // в полёте НЕ жжёт (урон — только импакт/наземный огонь)
                break;
            }
            case AT_FIRE: {                              // ОГОНЬ-ОБЪЕКТ (ZT 0x142c6): жжёт горючих в ~1кл; вечный(карта)/фитиль(огнемёт)
                ++a.frameT;
                if (a.fireCd > 0) { if (--a.fireCd <= 0) a.active = false; break; }  // ЗАТУХАНИЕ от пены: гаснет, не жжёт
                if ((a.frameT & 7) == 0) {               // периодический урон (не каждый кадр)
                    for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                          std::hypot(e.x - a.x, e.y - a.y) < 1.0) { e.burned = true; hitEnemy(e, 2, a.x, a.y); }
                    // игрока жжёт ТОЛЬКО хазард-огонь (state==0, карта 0x18), без огнеупор-костюма. Своё пламя (state 1) — нет.
                    // ⚠ РАДИУС ИГРОКА = 0x30/256 ≈ 0.19 кл (ZT 0x143cc cmpi #$30) — НАМНОГО меньше, чем врагам (1 кл 0x100):
                    // к огню можно подойти ВПЛОТНУЮ и почти не гореть (фикс «можно подойти ближе»).
                    if (a.state == 0 && !player().fireImmune && a.floor == cam.floor &&
                        std::hypot(cam.px - a.x, cam.py - a.y) < 0.19) damagePlayer(8, cam.px, cam.py, a.x, a.y);
                }
                if (a.timer >= 0 && --a.timer <= 0) a.active = false;          // фитиль (огнемёт); карта (timer<0) — вечно
                break;
            }
            case AT_FOAM: {                              // ПЕНА огнетушителя: ПАДАЕТ вниз, ТУШИТ огонь, БЕЗ урона
                for (auto& e : v) if (e.active && (e.think == AT_FIRE || e.think == AT_FLAME) && e.floor == a.floor &&
                                      std::hypot(e.x - a.x, e.y - a.y) < 0.6) {
                    if (e.think == AT_FIRE && e.timer < 0)                       // вечный карта-огонь → запомнить как потушенный
                        fireExtinguished().insert(e.floor * 1024 + (int)e.y * 32 + (int)e.x);
                    if (e.think == AT_FIRE && e.fireCd <= 0) e.fireCd = 10;      // СВОЯ АНИМ-ЗАТУХАНИЯ: огонь гаснет ~10 кадров (не мгновенно)
                    else if (e.think == AT_FLAME) e.active = false;             // летящее пламя огнемёта — мгновенно
                }
                a.z += a.vz; a.vz -= 0.012;              // ПАДЕНИЕ вниз (ZT draw смещает пену вниз с возрастом)
                double nx = a.x + a.vx, ny = a.y + a.vy;
                int cx = (int)nx, cy = (int)ny;
                if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                    cellBlocks(lvl.cellType(a.floor, cx, cy)) || --a.timer <= 0) { a.active = false; }
                else { a.x = nx; a.y = ny; a.vx *= 0.9; a.vy *= 0.9; }
                break;
            }
            case AT_EXPLOSION: if (++a.timer >= A_EXPL_FRAMES)  a.active = false; break;
            case AT_SPARK:     if (++a.timer >= A_SPARK_FRAMES) a.active = false; break;
            case AT_DEATH:     if (++a.timer >= A_EXPL_FRAMES)  a.active = false; break;
            case AT_BLOOD: {                              // КРОВЬ (ZT think 0x158cc): лёт→гравитация→пятно о стену/пол
                if (a.state != 0) {                       // ПЯТНО (1=стена, 2=пол): АДАПТИВНОЕ испарение (ZT 0x159c6)
                    int liveB = 0; for (auto& b : v) if (b.active && b.think == AT_BLOOD) ++liveB;
                    // возраст += (активных+1)/2 за кадр → байт переполнился (≥256) → исчезло; больше крови = быстрее сохнет
                    // (1 шт ~256 кадров, 16 шт ~32). Пятно на СТЕНЕ сползает вниз: z −= шаг каждые 8 кадров (ZT −1/8тик).
                    if (a.state == 1 && (a.frameT++ & 7) == 0 && a.z > 0.06) a.z -= 0.02;
                    a.timer += (liveB + 1) / 2;
                    if (a.timer >= 256) a.active = false;
                    break;
                }
                bool hitWall = false;
                for (int s = 0; s < 16; ++s) {            // 16 суб-шагов по vel/16 с коллизией (ZT CCD, dbra d7=$f) — не проскочит тонкую стену
                    double nx = a.x + a.vx / 16.0, ny = a.y + a.vy / 16.0;
                    int cx = (int)nx, cy = (int)ny;
                    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                        cellBlocks(lvl.cellType(a.floor, cx, cy))) { hitWall = true; break; }
                    a.x = nx; a.y = ny;
                }
                a.z += a.vz; a.vz -= 0.03125;            // гравитация (ZT $24 += $2e; $2e -= 2 → /64)
                a.vx *= 0.75; a.vy *= 0.75;              // гориз. трение (ZT vel -= vel>>2 при отсутствии столкновения)
                if (hitWall)        { a.state = 1; a.vx = a.vy = a.vz = 0; a.tile = A_BLOOD_SPLAT; a.timer = 0; a.frameT = 0; }   // пятно о СТЕНУ ($1a=0x16f02)
                else if (a.z <= 0)  { a.z = 0; a.state = 2; a.vx = a.vy = a.vz = 0; a.tile = A_BLOOD_SPLAT; a.timer = 0; }        // пятно на ПОЛУ ($1a=0x16f2c)
                break;
            }
            case AT_MINE: {                              // детонация: враг ИЛИ игрок ближе 1 кл → взрыв + урон
                if (a.timer > 0) { --a.timer; break; }   // АРМИНГ (ZT $1f=0x14): пока инертна — НЕ детонирует (защита ставившего)
                bool trig = (a.floor == cam.floor && gameDist(cam.px - a.x, cam.py - a.y) < 1.0);  // ZT 0x100 = 1 кл (игрок)
                if (!trig) for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                                 gameDist(e.x - a.x, e.y - a.y) < 1.0) { trig = true; break; }  // или враг
                if (trig) { explodeAt(lvl, a.x, a.y, a.floor, cam, 0.0); a.active = false; }  // мина на ПОЛУ → взрыв низко
                break;
            }
            case AT_ENEMY: {                             // AI: ДИСПЕТЧ ПО КЛАССУ (по разбору capstone think-функций каждого типа)
                if (a.floor != cam.floor) break;
                if (a.gunBurst > 0) {                       // ОЧЕРЕДЬ выстрела: 0x68(FM)+0x97(PSG-шум) ×3 через ~120мс (MAME 0.1-0.15с). По времени — think на переменном FPS
                    uint32_t now = SDL_GetTicks();
                    if (now >= a.gunBurstNext) { snd::playSfx(0x68); snd::playSfx(0x97); --a.gunBurst; a.gunBurstNext = now + 120; }
                }
                // Sgt → FH-SF: трансформация человека в инопланетянина по истечении времени (ZT state6 0x1b51a).
                // Меняем srcType → класс остаётся EC_GRENADIER, а gfx-слот (Sgt→FH-SF) и спрайты сменяются автоматически.
                if (a.xformT > 0 && !enemiesFrozen()) {
                    if (--a.xformT == 0 && a.srcType == 0x29) {
                        a.srcType = 0x69; a.tile = enemyTileForCt(0x69);  // стал Former Human SF
                        a.state = 0; a.timer = 0; a.fireCd = 30;          // сброс боевого цикла после морфа
                    }
                }
                EClass ec = enemyClass(a.srcType);
                // BOSS3 ПРИТВОРЯЕТСЯ МЁРТВЫМ (ZT 0x1a3aa: state6, $35=0x19=25 тиков) — лежит неподвижно, затем ВОСКРЕСАЕТ
                // с HP=1000 (1/8, ZT 0x1a40c). state=99 ставится в hitEnemy при первой «смерти» (до этого revived=false).
                if (a.srcType == 0x6A && a.state == 99) {
                    a.vx = a.vy = 0;
                    if (--a.timer <= 0) {                               // 25 тиков истекли → встаёт
                        a.revived = true; a.hp = enemyHp(0x6A) / 8; a.state = 0; a.timer = 0; a.fireCd = 30;
                        spawnSparkA(a.x, a.y, a.floor);
                    }
                    break;
                }
                if (a.hitT > 0) {                                        // СТАГГЕР (ZT state 2, sub_01887a): выход по ЗАТУХАНИЮ скорости
                    double sp = std::hypot(a.vx, a.vy);
                    if (sp >= 0.039) {                                   // ZT 0x0a/256≈0.039 кл/кадр: ещё «летит» → скользим + ÷2, держимся в стаггере
                        double nx = a.x + a.vx, ny = a.y + a.vy;
                        if (!enemyBlockedAt(lvl.cellType(a.floor, (int)nx, (int)a.y), a.floor, (int)nx, (int)a.y, nx - (int)nx, a.y - (int)a.y)) a.x = nx;
                        if (!enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ny), a.floor, (int)a.x, (int)ny, a.x - (int)a.x, ny - (int)ny)) a.y = ny;
                        enemyPlayerStandoff(a, cam);                     // анти-оверлап и в стаггере (не слиться с игроком)
                        a.vx *= 0.5; a.vy *= 0.5;                        // ZT: asr.w (÷2) скорости каждый кадр
                        ++a.hitT;                                        // кадр анимации стаггера (растёт пока держится)
                        break;
                    }
                    // скорость СЕЛА (ZT $188b2): решаем смерть / восстановление
                    if (a.hp <= 0) {                                     // HP<0 → СМЕРТЬ → труп на месте (ZT $189e4, v уже мала)
                        int dr = enemyWeaponDrop(a.srcType);            // солдаты роняют ствол (невидимо; подбор шагом на труп)
                        if (dr == -2) dr = (enemyRng() & 0x10) ? 10 : 7;  // Sergeant/FH-SF: btst #4 → laser(10) иначе grenade(7) (дизасм 0x1b716)
                        spawnCorpse(a.x, a.y, a.floor, a.srcType, a.vx, a.vy, a.variant, dr, a.burned);  // burned → обугленный труп
                        a.active = false; break;
                    }
                    a.hitT = 0; a.vx = a.vy = 0;                         // ВЫЖИЛ → восстановление к обычному AI (ZT $34=0)
                    break;                                              // пауза кадр на восстановление
                }
                if (enemiesFrozen()) break;                              // чит: враги замерли
                double rx = cam.px - a.x, ry = cam.py - a.y;
                double de = std::hypot(rx, ry);                              // евклид — ТОЛЬКО для единичного вектора прицела
                double ux = (de > 0.01) ? rx / de : 0.0, uy = (de > 0.01) ? ry / de : 0.0;
                double d = gameDist(rx, ry);                                 // ГЕЙМПЛЕЙНАЯ дистанция (октаг. d7c0) — все пороги/урон ниже
                bool los = enemyLOS(lvl, a.floor, a.x, a.y, cam.px, cam.py);
                if (a.fireCd > 0) --a.fireCd;
                if (a.fireAnimT > 0) --a.fireAnimT;                      // таймер анимации стрельбы/удара
                EMove mv = enemyMove(a.srcType);
                // движение к точке (tx,ty): v=clamp((t-pos)·gain, ±vmax) per-axis, коллизия по осям → липнет к стенам
                auto moveTo = [&](double tx, double ty, double scale) {
                    double avx = (tx - a.x) * mv.gain, avy = (ty - a.y) * mv.gain, vm = mv.vmax * scale;
                    if (avx >  vm) avx =  vm; else if (avx < -vm) avx = -vm;
                    if (avy >  vm) avy =  vm; else if (avy < -vm) avy = -vm;
                    a.vx = avx; a.vy = avy;
                    // КОЛЛИЗИЯ С МАРЖОЙ (ZT 0x145aa/0x146fe клампят суб-позицию $21/$23 к [0x40,0xbf] = радиус ~0.25кл от стены):
                    // проверяем ВЕДУЩИЙ КРАЙ (pos+sign·R), иначе центр заходил в стену вплотную → спрайт врага обрезался стеной.
                    const double R = 0.26;
                    const bool canOpen = enemyOpensDoors(a.srcType);    // только 5 типов толкают двери (ZT b1c4/b202)
                    double nx = a.x + a.vx, ny = a.y + a.vy;
                    double ex = nx + (a.vx > 0 ? R : -R);               // передний край по X
                    if (a.vx != 0 && !enemyBlockedAt(lvl.cellType(a.floor, (int)ex, (int)a.y), a.floor, (int)ex, (int)a.y, ex - (int)ex, a.y - (int)a.y, canOpen)) a.x = nx;
                    double ey = ny + (a.vy > 0 ? R : -R);               // передний край по Y
                    if (a.vy != 0 && !enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ey), a.floor, (int)a.x, (int)ey, a.x - (int)a.x, ey - (int)ey, canOpen)) a.y = ny;
                    enemyPlayerStandoff(a, cam);                         // АНТИ-ОВЕРЛАП (ZT 0x146fe): не проваливаться в игрока
                    if (std::hypot(a.vx, a.vy) > 0.004) a.frameT += 2;   // кадр ходьбы (anim0) при движении
                };

                // ═══ DOG (0x19540): ДРЕМЛЕТ У ДОМА → погоня по ЖИВОМУ игроку при ОБНАРУЖЕНИИ → прыжок-укус ═══
                if (ec == EC_DOG) {
                    if (a.state == 0) {                                  // ПАТРУЛЬ У ДОМА (НЕ дрейф к игроку!): бродит ±1кл у спавна
                        if (los && d < 6.0) { a.state = 1; }             // ОБНАРУЖИЛ (LOS + близко) → погоня
                        else { moveTo(a.aimX, a.aimY, 0.28);              // патруль медленно (vmax≈0x14, ZT 0x1977c)
                            if (--a.timer <= 0) { a.aimX = a.homeX + ((int)(enemyRng() & 0x1ff) - 256) / 256.0;  // у ДОМА
                                                   a.aimY = a.homeY + ((int)(enemyRng() & 0x1ff) - 256) / 256.0; a.timer = 70; } }
                        break;
                    }
                    if (d > 11.0 && !los) { a.state = 0; a.timer = 1; break; }   // потерял → назад к дому
                    if (a.state == 1) { moveTo(cam.px, cam.py, 1.0);     // погоня — трек ЖИВОГО игрока, быстро
                        if (d < 1.2) { a.state = 2; a.timer = 14; a.aimX = cam.px; a.aimY = cam.py; } }
                    else { moveTo(a.aimX, a.aimY, 1.0);                  // прыжок к запомненной точке
                        if (d < 0.5 && a.fireCd <= 0) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireCd = 48; a.fireAnimT = 12; snd::playSfx(snd::enemyFireSfx(a.srcType)); }  // укус ≤0.5кл (ZT 0x19678 dist<0x80)
                        if (--a.timer <= 0) a.state = 1; }
                    break;
                }

                // ═══ HYDACA (0x14c2a): живёт на ПОТОЛКЕ (a.z=1, спрайт a5), ОТЦЕПЛЯЕТСЯ и ПАДАЕТ на пол (a.z→0, БЕЗ стены),
                //   на ПОЛУ (a.z=0, спрайт a0) краулит к игроку и БЫСТРО кусает; обратно на потолок — ТОЛЬКО лезя по СТЕНЕ
                //   (вертик. спрайт a1, animSt=4). НЕ умеет «всплыть» с пола без стены. ZT +0x24 высота / wall-check 0x150de. ═══
                if (ec == EC_HOPPER) {
                    if (a.state == 0) { if (los && d < 9.0) { a.state = 1; a.timer = 40; } else break; }   // ОБНАРУЖЕНИЕ
                    if (d > 16.0 && !los && a.vz == 0.0 && a.state < 3) { a.state = 0; a.vx = a.vy = 0; break; }  // потерял (не в лазании/полёте)
                    // ⭐ЦЕЛЬ = WANDER-точка (ZT re-aim 0x14bb4: aim = игрок + rnd±1кл), НЕ прямо к игроку → КРУЖИТ (укус→отворот→снова).
                    auto hydReaim = [&]{
                        a.aimX = cam.px + ((int)((enemyRng() >> 8) & 0xff) - 0x80) / 128.0;  // ±1 кл (ZT 0x14bfa: (byte−0x80)·2)
                        a.aimY = cam.py + ((int)((enemyRng() >> 8) & 0xff) - 0x80) / 128.0;
                        a.timer = (int)(d * 2.0) + (int)(enemyRng() & 0xf);                  // ZT $35 = dist>>7 + rnd&0xf
                    };
                    if (a.aimX == 0.0 && a.aimY == 0.0) hydReaim();
                    auto hydStep = [&](double vx, double vy){                                // латер. шаг с коллизией (без открытия дверей)
                        double nx = a.x + vx, ny = a.y + vy;
                        if (vx != 0 && !enemyBlockedAt(lvl.cellType(a.floor,(int)nx,(int)a.y), a.floor,(int)nx,(int)a.y, nx-(int)nx, a.y-(int)a.y, false)) a.x = nx;
                        if (vy != 0 && !enemyBlockedAt(lvl.cellType(a.floor,(int)a.x,(int)ny), a.floor,(int)a.x,(int)ny, a.x-(int)a.x, ny-(int)ny, false)) a.y = ny;
                    };
                    // КРАУЛ к aim БЕЗ R-марджина moveTo (иначе Hydaca стоит в 0.26кл от стены и НЕ доходит до края для климба;
                    // ZT move 15246 — вплотную). Скорость как enemyMove, коллизия по клетке → достаёт до грани стены.
                    auto hydCrawl = [&](double scale){
                        double vmx = mv.vmax * scale;
                        double avx = (a.aimX - a.x) * mv.gain, avy = (a.aimY - a.y) * mv.gain;
                        if (avx >  vmx) avx =  vmx; else if (avx < -vmx) avx = -vmx;
                        if (avy >  vmx) avy =  vmx; else if (avy < -vmx) avy = -vmx;
                        a.vx = avx; a.vy = avy;                                   // ⭐скорость для directional-анимации пола (enemyDirIndex) — иначе dir всегда 0=фронт
                        hydStep(avx, avy); enemyPlayerStandoff(a, cam);
                    };
                    // ═══ КЛИМБ (states 3-10 = ZT 3-A): ФИКС. латераль ±0x28 вдоль стены + Z ползёт; стена кончилась → СРЫВ с латер.
                    //   пушем ОТ стены (ZT 15086/…). Проба стены на 0x24 в её сторону (ZT 0x1521e = celltype LUT==1). ═══
                    if (a.state >= 3 && a.state <= 10) {
                        static const double CW = 36.0/256.0, CV = 40.0/256.0;               // 0x24 проба, 0x28 скорость
                        struct HC { double wdx, wdy, lvx, lvy, fvx, fvy; };                  // проба-стены / латераль / срыв
                        static const HC C[8] = {
                            {  0,-CW,  CV,  0,   0, CV}, {  0,-CW, -CV,  0,   0, CV},         // 3,4: N стена → E/W, срыв S
                            {  0, CW,  CV,  0,   0,-CV}, {  0, CW, -CV,  0,   0,-CV},         // 5,6: S стена → E/W, срыв N
                            { CW,  0,   0,-CV, -CV,  0}, { CW,  0,   0, CV, -CV,  0},         // 7,8: E стена → N/S, срыв W
                            {-CW,  0,   0,-CV,  CV,  0}, {-CW,  0,   0, CV,  CV,  0},         // 9,10: W стена → N/S, срыв E
                        };
                        const HC& c = C[a.state - 3];
                        a.patrolDir = (uint8_t)((a.state - 3) / 2);                          // сторона стены N/S/E/W (для анимации)
                        bool wall = hydClimbWall(lvl.cellType(a.floor, (int)std::floor(a.x + c.wdx), (int)std::floor(a.y + c.wdy)));  // ZT 1521e (не диагональ)
                        if (!wall) { a.vx = c.fvx; a.vy = c.fvy; a.vz = -0.0001; a.state = 1; a.frameT += 2; break; }  // стена кончилась → СРЫВ (ZT 15086)
                        hydStep(c.lvx, c.lvy);                                               // ползёт вдоль стены (ZT фикс. ±0x28)
                        a.z += a.vz;                                                         // Z ползёт (ZT $24 += $2e)
                        if (a.vz > 0 && a.z >= 1.0) { a.z = 1.0; a.variant = 1; a.state = 1; a.vz = 0; hydReaim(); a.fireCd = 25; }        // долез до ПОТОЛКА → st1 (+кулдаун краула, чтоб выползти из угла)
                        else if (a.vz < 0 && a.z <= 0.0) { a.z = 0.0; a.variant = 0; a.state = 1; a.vz = 0; hydReaim(); a.fireCd = 25; }   // спустился на ПОЛ → st1 (+кулдаун)
                        a.frameT += 2; break;
                    }
                    // ═══ ПАДЕНИЕ (a.vz != 0, ZT state2 0x14ca6): гравитация + ЛАТЕРАЛЬ (15246 move + ½-декей) ═══
                    if (a.vz != 0.0) {
                        a.z += a.vz; a.vz -= 3.0 / 48.0;                 // гравитация (ZT 0x14ca6: $2e−=3)
                        hydStep(a.vx, a.vy); a.vx *= 0.5; a.vy *= 0.5;   // ЛАТЕРАЛЬ вдоль вектора срыва + затухание ×½ (ZT 14cdc move+asr)
                        if (a.z <= 0.0) {                               // ПРИЗЕМЛЕНИЕ на пол (ZT $24=−24 → state0)
                            a.z = 0.0; a.vz = 0.0; a.variant = 0;
                            if (a.hp <= 0) { spawnCorpse(a.x, a.y, a.floor, a.srcType, 0, 0, a.variant, -1, a.burned); a.active = false; break; }  // сбита насмерть → труп
                            a.timer = 60 + (int)(enemyRng() % 50);
                        }
                        a.frameT += 2; break;
                    }
                    // ═══ КРАУЛ (state 1): вход в климб у стены (1/8/тик) → wander-краул + укус (пол) / отцеп (потолок) ═══
                    int wallDir = hydacaWallDir(lvl, a.floor, a.x, a.y);      // стена вплотную (0=W,1=E,2=N,3=S); −1 нет
                    // ВХОД В КЛИМБ (ZT 150de/клоны): у стены + 1/8 rng → climb-стейт по (сторона стены, латераль к aim), z-vel rnd(2..5).
                    auto tryClimb = [&]() -> bool {
                        if (wallDir < 0 || a.fireCd > 0 || (enemyRng() & 0xe0) != 0) return false;   // 1/8 шанс/тик (ZT rng&0xe0==0); кулдаун после климба/укуса = окно краула (выползти из угла)
                        int st;
                        if      (wallDir == 2) st = (a.aimX > a.x) ? 3 : 4;          // N стена → E(3)/W(4) к aim (ZT 1517e)
                        else if (wallDir == 3) st = (a.aimX > a.x) ? 5 : 6;          // S стена → E/W (ZT 151ce)
                        else if (wallDir == 1) st = (a.aimY > a.y) ? 8 : 7;          // E стена → S(8)/N(7) (ZT 1512e)
                        else                   st = (a.aimY > a.y) ? 10 : 9;         // W стена → S(10)/N(9) (ZT 150de)
                        a.state = st;
                        double vz = (2 + (int)(enemyRng() % 4)) / 48.0;             // z-vel rnd(2..5)
                        a.vz = (a.z < 0.5) ? vz : -vz;                             // пол → ВВЕРХ / потолок → ВНИЗ (ZT знак по $24)
                        return true;
                    };
                    if (tryClimb()) { a.frameT += 2; break; }                        // вошёл в климб → в этот тик НЕ двигаемся (ZT rts)
                    if (a.z > 0.5) {                                     // НА ПОТОЛКЕ
                        a.variant = 1;
                        hydCrawl(0.8);
                        if ((enemyRng() & 0x7f) == 0) a.vz = -0.0001;    // ZT 0x14d2c: 1/128/тик отцеп → падение
                        else if (--a.timer <= 0) hydReaim();
                    } else {                                            // НА ПОЛУ
                        a.variant = 0;
                        hydCrawl(1.0);
                        if (d < 0.39 && a.fireCd <= 0) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireCd = 10; a.fireAnimT = 10;
                                                         snd::playSfx(snd::enemyFireSfx(a.srcType)); }  // укус (ZT 0x14ee6 cd $41=0xa, урон 0x2bc→6)
                        if (--a.timer <= 0) hydReaim();
                    }
                    a.frameT += 2;
                    break;
                }

                // ═══ REVENANT (0x1a92a): СЕТОЧНЫЙ ПАТРУЛЬ (идёт ПРЯМО в одном из 8 направлений, на клетке поворачивает —
                //   НЕ движется к игроку!) → при ОБНАРУЖЕНИИ (LOS+радиус) РЫВОК к игроку, контакт-урон. ZT нав 0x1ac76
                //   (8 направлений по state, ±0x10, ре-навигация в центре клетки). Юзер: «идёт по линейной траектории». ═══
                if (ec == EC_RANGED_FAST) {
                    // ZT нав 0x1ac76: 8 направлений (N,NW,W,SW,S,SE,E,NE = вектор ±0x10), ДЕТЕРМИНИРОВАННЫЙ патруль —
                    // идёт ПРЯМО пока в центре клетки не упрётся, тогда поворот на СЛЕД. направление (state+1 mod 8), НЕ случайно.
                    static const int RDX[8] = { 0,-1,-1,-1, 0, 1, 1, 1}, RDY[8] = {-1,-1, 0, 1, 1, 1, 0,-1};
                    if (a.state == 0) {                                  // ПАТРУЛЬ
                        // ЗТ детект: dist ≤ 0x12c=1.17кл НАПРЯМУЮ (0x1ac68) ИЛИ ≤ 0x180=1.5кл + LOS (look-ahead 0x1ad72).
                        // Было 2.6кл «по просьбе» → юзер: «несётся втупую издалека» → возвращено к дизасм-значениям.
                        if (d < 1.17 || (los && d < 1.5)) {              // ДЕТЕКТ (проксимити, ZT 0x1ac68/0x1ad72)
                            snd::playSfx(0x71); a.state = 1;
                            // ⭐ПАК-АЛЕРТ (ZT 0x1ab6c): детект ОДНОГО будит ВСЕХ Revenant'ов в 4кл (0x400) → в бой (тот же игрок).
                            // Триггер = ДЕТЕКТ, НЕ попадание: подстреленного издалека (hHit) группа не слышит — можно снять по одному.
                            for (Actor& o : actors()) {
                                if (&o == &a || !o.active || o.think != AT_ENEMY || o.srcType != 0x66 || o.floor != a.floor || o.state >= 8) continue;
                                if (gameDist(o.x - a.x, o.y - a.y) <= 4.0) o.state = 1;  // → revEngage в их think
                            }
                        }
                        else {
                            double tdx = a.aimX - a.x, tdy = a.aimY - a.y;
                            if ((a.aimX == 0 && a.aimY == 0) || std::hypot(tdx, tdy) < 0.15) {  // достиг клетки → ре-навигация
                                int cx = (int)a.x, cy = (int)a.y;
                                auto walk = [&](int dir){ int tx = cx + RDX[dir], ty = cy + RDY[dir];
                                    uint8_t wct = lvl.cellType(a.floor, tx, ty);
                                    if (cellBlockedAt(wct, 0.5, 0.5)) return false;
                                    if (cellIsDoor(wct) && doorOpen(a.floor, tx, ty) < 0.4) return false;  // Revenant НЕ открывает дверь → закрытая блокирует (иначе застрянет)
                                    return true; };
                                int nd = -1;                              // ZT 0x1acfa: продолжай ПРЯМО (patrolDir); упор → поворот +1 mod 8 до проходимого
                                for (int k = 0; k < 8; k++) { int dir = (a.patrolDir + k) & 7; if (walk(dir)) { nd = dir; break; } }
                                if (nd < 0) { a.aimX = a.x; a.aimY = a.y; a.timer = 10; }       // заперт — стоит
                                else { a.patrolDir = (uint8_t)nd; a.aimX = (cx + RDX[nd]) + 0.5; a.aimY = (cy + RDY[nd]) + 0.5; }
                            }
                            moveTo(a.aimX, a.aimY, 0.30);                // медленно ПО ПРЯМОЙ к клетке (линейная траектория)
                            break;
                        }
                    }
                    // ═══ COMBAT (ZT states 8/9): НЕ прямой бег к игроку! engage 1abe0 → рывок к AIM-точке (игрок+упреждение+
                    //   разброс ±1кл), контакт-урон в approach; в aim-окне ОДИН выстрел. Цикл: рывок→прицел→выстрел→ре-engage.
                    //   ⚠ Раз вступил в бой — БОЛЬШЕ НЕ ВОЗВРАЩАЕТСЯ в патруль (ZT: state≥8 не сбрасывается, только смерть/стаггер) →
                    //   держится aggro (виден на радаре), преследует относ. неотвязно (робот). Прежний disengage d>4 = НЕфейтфул, снят. ═══
                    // engage/re-engage (ZT 0x1abe0): новая AIM = игрок + упреждение(скорость) + rnd±0x100(±1кл); state 8, timer=(dist>>7)+rnd&0xf
                    auto revEngage = [&]{
                        uint32_t r1 = enemyRng(), r2 = enemyRng();
                        double ox = ((int)((r1 >> 9) & 0x1ff) - 256) / 256.0;   // ±1 кл (ZT 0x1ac20: &0x1ff −0x100)
                        double oy = ((int)((r2 >> 9) & 0x1ff) - 256) / 256.0;
                        a.aimX = cam.px + pvx * 4.0 + ox;                       // + скорость игрока (упреждение, ZT +$2a/$2c)
                        a.aimY = cam.py + pvy * 4.0 + oy;
                        a.state = 8; a.timer = (int)(d * 2.0) + (int)(enemyRng() & 0xf);  // ZT dist>>7 = d·2(кл) + rnd 0..15
                    };
                    if (a.state == 1) revEngage();                       // только что обнаружен (патруль→бой) → первый engage
                    moveTo(a.aimX, a.aimY, 1.0);                          // РЫВОК к AIM-точке (±0x40, ZT 0x1a9aa/0x1a9b0), НЕ к игроку
                    if (a.state == 8) {                                   // APPROACH: контакт-урон + переход к прицелу
                        if (d < 0.25) { damagePlayer(2, cam.px, cam.py, a.x, a.y); snd::playSfx(0x1b); }  // контакт ≤0x40 (ZT 0x1a9f6 d0=0x384→2) + звук 0x1b (i-frame гейтит)
                        double da = std::hypot(a.aimX - a.x, a.aimY - a.y);
                        if (da < 0.15 || --a.timer <= 0) { a.state = 9; a.timer = 10; a.fireAnimT = 0; }  // достиг aim / таймер → ПРИЦЕЛ 10т ($35=0xa)
                    } else {                                              // state 9 AIM/FIRE (ZT 0x1aa28): движется к aim, ОДИН выстрел на tick6
                        int t = --a.timer;                                // 9,8,...,0
                        if (t == 6) {                                     // ZT $35==6 → ВЫСТРЕЛ (hitscan лазер)
                            a.fireAnimT = 12;
                            if (los) {
                                bool cm = enemyShotMiss(lvl.env(a.floor));    // стелс-ролл (стойка+тьма, ZT 0x1aa76 — как FH)
                                if (!cm && d < 2.0) { int rd = 16 - (int)(d * 8.0); if (rd < 1) rd = 1; damagePlayer(rd, cam.px, cam.py, a.x, a.y); }  // урон 16−(dist·2>>6), ≤2кл (ZT 0x1aac4)
                                snd::playSfx(0x1e);                       // лазер (hit ИЛИ miss, ZT 0x1aad8)
                            }
                        } else if (t == 5 || t == 4) snd::playSfx(0x1e);  // ZT $35==5/4 → заряд-звук 0x1e
                        if (t <= 0) revEngage();                          // aim-окно кончилось → ре-engage (новый рывок к новой aim)
                    }
                    break;
                }

                // ═══ ОБЩАЯ aim-wander (дальние/гренадёр/Imp/боссы) ═══
                bool melee = (ec == EC_MELEE || ec == EC_HOPPER);
                auto reaim = [&]() {                                     // ZT 0x18334: прицел = игрок + (упреждение) + rnd
                    // FH-SF (0x69, ZT 0x19aa2): игрок ДАЛЕКО (dist≥4кл=0x400) → идёт ПРЯМО к нему (aim=игрок, без wander),
                    // $35=0xf (длинный advance) — агрессивное сближение; близко → обычный wander+упреждение.
                    if (a.srcType == 0x69 && d >= 4.0) {
                        a.aimX = cam.px; a.aimY = cam.py; a.state = 1; a.timer = 15; return;
                    }
                    bool boss3 = (a.srcType == 0x6A);                    // Boss3 (ZT 0x1a3cc): aim=игрок+rnd±0x80 (±0.5кл) БЕЗ упреждения — точный чардж в упор
                    uint32_t r1 = enemyRng(), r2 = enemyRng();
                    double mag = (melee || boss3) ? 0.5 : 1.0;          // Imp/Hydaca/Boss3 ±0.5кл; прочие дальние/боссы ±1кл
                    double offx = ((int)((r1 >> 9) & 0x1ff) - 256) / 256.0 * mag;
                    double offy = ((int)((r2 >> 9) & 0x1ff) - 256) / 256.0 * mag;
                    bool lead = !melee && !boss3;                        // дальние/Boss1/Boss2 упреждают; Boss3 — нет (ZT rnd±0x80 без vel)
                    a.aimX = cam.px + (lead ? pvx * 6.0 : 0.0) + offx;
                    a.aimY = cam.py + (lead ? pvy * 6.0 : 0.0) + offy;
                    // advance-таймер ПО ДИСТАНЦИИ (ZT 0x18334 `$35 = dist>>6 + rnd`): чем ДАЛЬШЕ враг, тем дольше
                    // наступает между перецеливаниями; вблизи — чаще (быстрее стреляет). dist>>6 = distКл·4 (FH/Sgt/
                    // FH-SF/Imp, rnd&0x1f=0..31); боссы dist>>7 = distКл·2 (rnd&0xf=0..15). Было фикс 35-70 (не по дист).
                    int base, rmax;
                    if (ec == EC_BOSS_MELEE || ec == EC_BOSS_PROJ) { base = (int)(d * 2.0); rmax = 16; }   // ZT dist>>7
                    else                                          { base = (int)(d * 4.0); rmax = 32; }   // ZT dist>>6
                    a.state = 1; a.timer = base + (int)(enemyRng() % rmax);
                    if (a.timer < 6) a.timer = 6;                        // минимум (вплотную не 0)
                };
                if (a.state == 0) {                                     // ОБНАРУЖЕНИЕ (LOS + ≤8кл): до того стоит
                    if (los && d < 8.0) { reaim(); }                    // (детект Revenant/0x71 — в его блоке @строка 974; сюда EC_RANGED_FAST не доходит)
                    else break;
                }
                if (d > 14.0 && !los) { a.state = 0; a.vx = a.vy = 0; break; }
                // Revenant («робот», ZT 0x1ac68): ВДАЛИ (d>1.2кл = 0x12c) крадётся МЕДЛЕННО (шаг ±0x10), рывок ±0x40
                //   только ВБЛИЗИ → «не несётся издали, атакует только когда близко» (юзер verif. трейсом).
                double mscale = (ec == EC_RANGED_FAST && d > 1.2) ? 0.25 : 1.0;
                moveTo(a.aimX, a.aimY, mscale);
                bool bossCls = (ec == EC_BOSS_PROJ || ec == EC_BOSS_MELEE);
                if (--a.timer <= 0) {                                    // state1 advance → state2 fire-окно → reaim
                    if (a.state == 1) { a.state = 2; a.timer = bossCls ? 60 : 50; }
                    else reaim();
                }
                // ── АТАКА: РОВНЫЙ интервал = fireCd (РАЗВЯЗАНО от фаз движения; раньше огонь требовал state2+timer≤25,
                //    а при fireCd>цикла окна пропускались → атаки «скакали». Теперь cd ставится ТОЛЬКО при выстреле в радиусе). ──
                (void)bossCls;
                if (a.fireCd <= 0 && los) {
                    bool crouchMiss = enemyShotMiss(lvl.env(a.floor));  // стелс-гейт ZT: присед+тьма снижают шанс попадания
                    int fa0 = a.fireAnimT;                       // для детекции «только что выстрелил» (fireAnimT→12)
                    switch (ec) {
                        case EC_MELEE:                                   // Imp: контакт ≤0.39кл (ZT 0x189c2 dist<0x64), урон 12 (d0=0x100)
                            if (d < 0.39) { damagePlayer(12, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 22;
                                            snd::playSfx(snd::enemyFireSfx(a.srcType)); } break;  // звук укуса 0x94 (ZT 0x189da d740)
                        case EC_RANGED_FAST:                             // Revenant: контакт-рывок ≤1.6кл (ZT engage 0x12c)
                            if (d < 1.6 && !crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 30; } break;
                        case EC_RANGED:                                  // FH: HITSCAN ≤4кл, falloff 16−дист·4
                            // ZT 0x18452: при LOS ВСЕГДА проигрывает выстрел (анимация+звук на substep5), урон только
                            // при попадании (rng<стелс-порог). Промах (присед/тьма) = видимый выстрел БЕЗ урона, НЕ молчание.
                            if (d < 4.0) { if (!crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); } a.fireAnimT = 12; a.fireCd = 55; a.gunBurst = 3; a.gunBurstNext = 0; } break;  // ЗВУК ВЫСТРЕЛА FH = 0x68(FM)+0x97(PSG-шум) на КАЖДЫЙ выстрел (ZT $35==3/4 @18442; урон при попадании)
                        case EC_GRENADIER: {                             // Sgt/FH-SF: hitscan ЛИБО граната (ВЗАИМОИСКЛ., ZT state1 vs state5)
                            // Шанс гранаты: Sgt 1/8 (ZT 0x1b2aa andi #7), FH-SF 1/16 (0x19c5a andi #f); дист≥1кл + LOS; иначе выстрел.
                            int gmask = (a.srcType == 0x29) ? 7 : 15;    // Sgt кидает вдвое чаще спецназовца-пришельца
                            if (d >= 1.0 && (enemyRng() & gmask) == 0) { spawnGrenade(a.x, a.y, a.floor, ux, uy, 1); a.fireAnimT = 12; a.fireCd = 55; snd::playSfx(0x1b); }  // ГРАНАТА (дуга+фитиль) + звук БРОСКА 0x1b (ZT @019d7c/1b3cc, ТОЛЬКО на бросок)
                            else if (d < 4.0) { if (!crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); } a.fireAnimT = 12; a.fireCd = 55; a.gunBurst = 3; a.gunBurstNext = 0; }  // ЗВУК ВЫСТРЕЛА FH-SF/Sgt = 0x68+0x97 (ZT @19cc2/1b312) на КАЖДЫЙ hitscan-выстрел
                            } break;
                        case EC_BOSS_PROJ:                               // Boss1: САМОНАВОДЯЩИЙСЯ снаряд
                            if (d < 12.0) { spawnEnemyShot(a.x, a.y, a.floor, ux, uy, 1); a.fireAnimT = 12; a.fireCd = 60; } break;
                        case EC_BOSS_MELEE:                              // Boss3(0x6A) / Boss2(0x6B) — оба гейтятся стелсом (присед/промах)
                            if (a.srcType == 0x6A) {                     // Boss3: меле ≤0.5кл d0=0x100→12 (ZT 0x1a5c4 dist<0x80);
                                if (a.hp < 20 && (enemyRng() & 1) && d < 4.0) {   // при HP<20 (ZT 0x7d0) ~50% — ДАЛЬНИЙ d0=0x1f4→9 (ZT 0x1a5e6)
                                    if (!crouchMiss) damagePlayer(9, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 45;
                                } else if (d < 0.5) { if (!crouchMiss) damagePlayer(12, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 45; }
                            } else {                                     // Boss2: hitscan ≤4кл d0=0x1f4→9 (ZT 0x18dec)
                                if (d < 4.0) { if (!crouchMiss) damagePlayer(9, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 45; }
                            } break;
                        default: break;
                    }
                    if (a.fireAnimT > fa0 && a.floor == cam.floor &&        // ВРАГ ТОЛЬКО ЧТО ВЫСТРЕЛИЛ/УДАРИЛ → звук атаки врага
                        (ec == EC_RANGED_FAST || ec == EC_BOSS_PROJ || ec == EC_BOSS_MELEE))  // FH(RANGED)/FH-SF/Sgt(GRENADIER) hitscan МОЛЧИТ (был только 0x68+0x97 на попадании); граната-бросок звучит выше
                        snd::playSfx(snd::enemyFireSfx(a.srcType));
                }
                break;
            }
            case AT_ENEMY_SHOT: {                        // снаряд врага (Boss1 прямой / FH-SF граната): летит → урон / искра
                // Boss1-снаряд (state 1) ЛЕТИТ ПО ПРЯМОЙ: ZT 0x15f6e не пересчитывает скорость $2a/$2c (задана при
                // спавне) — «самонаведение» было ошибкой разбора; снаряд УКЛОНЯЕМ. state 2 = граната FH-SF (дуга).
                if (a.state == 2) {                                     // ГРАНАТА (FH-SF): дуга — z вверх + гравитация
                    a.z += a.vz; a.vz -= 0.004; if (a.z < 0) a.z = 0;
                } else {                                                // Boss1-снаряд: Z-дрейф ±0x1c (ROM 15f6e 15f72-15f90)
                    a.z += a.vz; if (a.z > 28.0) a.z = 28.0; else if (a.z < -28.0) a.z = -28.0;
                }
                if (a.floor == cam.floor && gameDist(cam.px - a.x, cam.py - a.y) < 0.5) {     // достал игрока (октаг. ≤0x40)
                    // ⭐ВЕРТИКАЛЬНЫЙ ДОЖ (ROM 15ff4-16012): низкий снаряд (Z<−8) ПЕРЕПРЫГИВАЕШЬ (питч>0=прыжок);
                    // высокий (Z>+8) ПОДНЫРИВАЕШЬ (питч<0=присед); Z∈[−8,+8] или стойка (питч≈0) → попадание всегда.
                    bool dodged = (a.state != 2) &&
                                  ((a.z < -8.0 && cam.pitch > 8.0) || (a.z > 8.0 && cam.pitch < -8.0));
                    if (!dodged) { damagePlayer(9, cam.px, cam.py, a.x, a.y); a.active = false; break; }  // попал → урон 9 + ранение 0x68
                    // увернулся — снаряд летит дальше (не гасим, пролетит мимо/в стену)
                }
                double nx = a.x + a.vx, ny = a.y + a.vy;
                int cx = (int)nx, cy = (int)ny;
                uint8_t sct = (cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H) ? lvl.cellType(a.floor, cx, cy) : 1;
                if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                    cellBlockedAt(sct, nx - cx, ny - cy) || cellIsDoor(sct) || --a.timer <= 0) {  // дверь тоже останавливает
                    spawnSparkA(a.x, a.y, a.floor); a.active = false;
                } else { a.x = nx; a.y = ny; }
                break;
            }
            case AT_CORPSE: {                            // труп СКОЛЬЗИТ от смертельного отлёта, затухает, упирается в стены
                ++a.frameT;                                          // тик анимации трупа (FH мерцает по LUT, Hydaca «ножки» RNG)
                if (a.timer > 0 && std::abs(a.vx) + std::abs(a.vy) > 0.004) {
                    double nx = a.x + a.vx, ny = a.y + a.vy;
                    if (!enemyBlockedAt(lvl.cellType(a.floor, (int)nx, (int)a.y), a.floor, (int)nx, (int)a.y, nx - (int)nx, a.y - (int)a.y)) a.x = nx;
                    if (!enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ny), a.floor, (int)a.x, (int)ny, a.x - (int)a.x, ny - (int)ny)) a.y = ny;
                    a.vx *= 0.5; a.vy *= 0.5; --a.timer;                 // затухание ÷2/кадр (ZT asr.w скорости стаггера)
                }
                break;
            }
            default: break;
        }
    }

    // ОЧЕРЕДЬ СПРАЙТОВ: каждый актёр → билборд в worldFx (рисуется drawDecorBillboards с z-тестом по стенам).
    auto& fx = worldFx(); fx.clear();
    for (const Actor& a : v) {
        if (!a.active) continue;
        switch (a.think) {
            case AT_EXPLOSION: case AT_DEATH: {            // взрыв: БЫСТРО раскрывается (2 кадра) → сжимается (квадрат)
                int t = a.timer, sz = (t < 2) ? (4 + t * 4) : (12 - (t - 2) * 2); if (sz < 2) sz = 2;
                int c = (int)(8 - a.z * 16);               // ВЫСОТА очага: z=0 пол (низ экрана) … z=0.5 ур.глаз (горизонт)
                fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz}); break; }
            case AT_SPARK: { uint8_t t = A_SPARK_SEQ[a.timer / 2 < 3 ? a.timer / 2 : 2];               // анимация 63→64→65
                int sz = 3 + a.timer; fx.push_back({a.x, a.y, a.floor, t, (int8_t)(-sz/2), (uint8_t)sz, (uint8_t)sz}); break; }  // мелкая, квадрат
            case AT_BLOOD: {                                // КРОВЬ (ZT draw 0x159fe): размеры в 1/16 S (frac16 = 16·пиксель/d5, где d5=scale=S).
                // eda0: d0=высота, d4=ширина (пиксели). ⭐СВЕРЕНО 2026-07-14: было base=4 квадратом (в 2–4× крупнее+не тот аспект).
                if (a.state == 0) {                            // ЛЁТ (0x15a9e): W=d5/8→frac16 2, H=d5/4→frac16 4 — капля ВЕРТИКАЛЬНАЯ 1:2
                    int c = (int)(8 - a.z * 16);
                    fx.push_back({a.x, a.y, a.floor, A_BLOOD_FLY, (int8_t)(c - 2), 4, 2}); }   // h=4, w=2
                else {
                    int age = a.timer; if (age > 255) age = 255;    // возраст $35 = 0..255
                    int w, h;
                    if (a.state == 2) {                             // ПОЛ (0x15a2c): W=(age+0x80)/64 растёт (лужа шире), H=(0x100−age)/256 площится
                        w = (128 + age) / 64;                       // frac16: age0=2 → age255≈6
                        h = (256 - age) / 256;                      // frac16: age0=1 → ~0 (ПЛОСКАЯ)
                    } else {                                        // СТЕНА (0x15a68): H=(age+0x100)/128 тянется вниз (потёк), W=(0x100−age)/256 сужается
                        h = (256 + age) / 128;                      // frac16: age0=2 → age255≈4
                        w = (256 - age) / 256;                      // frac16: age0=1 → ~0 (УЗКИЙ)
                    }
                    if (w < 1) w = 1; if (h < 1) h = 1;
                    int c = (a.state == 2) ? 8 : (int)(8 - a.z * 16);   // пол: на полу (c=8); стена: на высоте z (сползает)
                    fx.push_back({a.x, a.y, a.floor, A_BLOOD_SPLAT, (int8_t)(c - h/2), (uint8_t)h, (uint8_t)w}); }
                break; }
            case AT_FLAME: { int f = a.timer & 1;          // 2-кадровое МЕРЦАНИЕ размера (ROM draw 14150 @14170: tick&1 каждый кадр)
                int sz = 3 + a.timer / 3 + f;              // пламя в полёте, мелкое, квадрат, мерцает
                int c = (int)(8 - a.z * 16);               // экранная высота по Z: арка из ствола (z≈0.45) вниз к полу (z=0)
                fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz}); break; }
            case AT_FIRE: {                                // ОГОНЬ на полу (ZT draw 14272 / anim 14292): МЕРЦАЕТ КАЖДЫЙ кадр + СЖИМАЕТСЯ с возрастом
                int frame = a.frameT & 1;                 // тайл N / N+1 по (tick&1) — КАЖДЫЙ кадр (было >>2 = вчетверо медленно)
                uint8_t t = frame ? (uint8_t)(A_FIRE_TILE + 1) : A_FIRE_TILE;
                int sz = 8 + frame;
                if (a.timer >= 0) sz = sz * (a.timer < A_FIRE_LIFE ? a.timer : A_FIRE_LIFE) / A_FIRE_LIFE + 1;  // ROM: сжимается по (0x10−$1e) (только огнемёт-огонь, не вечный карта-хазард timer<0)
                if (a.fireCd > 0) sz = sz * a.fireCd / 10; // ЗАТУХАНИЕ от пены
                if (sz < 1) sz = 1; int c = 8 - sz;        // низом на полу
                fx.push_back({a.x, a.y, a.floor, t, (int8_t)c, (uint8_t)sz, (uint8_t)(sz > 1 ? sz - 1 : 1)}); break; }
            case AT_FOAM: {                                // ПЕНА: ФОРМА декор-тайла 0 (ZT draw 0x14272), но СИНЕ-БЕЛАЯ палитра (ZT
                int sz = 3 + a.timer / 2; if (sz > 9) sz = 9; if (sz < 2) sz = 2;   // line 0x20d2 — не огненная 0x20f2); сжимается с возрастом
                int c = (int)(7 - a.z * 16);                                        // ПАДАЕТ: z↓ → ниже по экрану
                WorldFx wf{a.x, a.y, a.floor, 0, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz};
                wf.foam = true; fx.push_back(wf); break; }                          // foam=true → рендер тайла 0 в пене-палитре
            case AT_BULLET:    fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)-1, 2, 2}); break;
            case AT_ENEMY_SHOT:                          // снаряд врага: граната (state2) — крупнее + дуга по z; иначе мелкий
                if (a.state == 2) { int sz = 5, c = (int)(2 - a.z * 16);
                    fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz}); }
                else { int c = (int)(-2 - a.z * 0.25);   // Boss1: экранная высота по Z-полосе (высокий Z=выше → перепрыгнуть/поднырнуть видно)
                       fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)c, 3, 3}); }
                break;
            case AT_GRENADE: { int c = (int)(8 - a.z * 16);   // граната летит по дуге (выше при z>0); ZT draw 0x13f3e: тайл 16, ~d5/4
                fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)(c - 2), 5, 5}); break; }   // ~0.3кл квадрат (был тайл13 sz6 — неверно)
            case AT_MINE: {                              // МИНА на полу: ZT draw 0x1402e = декор-тайл 8↔9 АНИМАЦИЯ (мигает),
                uint8_t mt = (uint8_t)(8 + ((decorFrame() >> 2) & 1));  // bit1 счётчика -$7149 → чередование 8/9
                fx.push_back({a.x, a.y, a.floor, mt, (int8_t)6, 3, 6}); break; }  // ПЛОСКАЯ 2:1 на полу (ZT_OBJECT_ASPECT мина=2:1, было «вытянуто вверх»)
            case AT_ENEMY: {                              // реальный спрайт врага: ХОДЬБА(по направл)/СТРЕЛЬБА/СТАГГЕР
                int slot = enemyGfxSlot(a.srcType);
                if (slot >= 0 && slot < 16 && g_enemyAnim[slot].ok) {
                    EnemyAnimSet& A = g_enemyAnim[slot];
                    uint8_t animSt = (a.hitT > 0) ? 2 : (a.fireAnimT > 0 ? 1 : 0);     // стаггер > стрельба/удар > ходьба
                    if (a.srcType == 0x6A && a.state == 99) animSt = 3;               // Boss3 притворяется мёртвым → death-поза (лежит)
                    else if (a.srcType == 0x65 && a.state >= 3 && a.hitT <= 0 && !A.climb.empty()) animSt = 4;  // Hydaca ЛАЗАНИЕ (states 3-10) → DIRECTIONAL вертик. поза (ZT draw states 3-A)
                    else if (a.srcType == 0x65 && std::fabs(a.vz) > 0.001 && a.hitT <= 0 && !A.climb.empty()) animSt = 5;  // Hydaca ПАДЕНИЕ (баллистика vz≠0) → a1(вниз)/a8(вверх-срыв)/a10(мёртв) (ZT draw state2), НЕ directional
                    if (a.srcType == 0x68 && a.state >= 1 && a.hitT <= 0 && a.fireAnimT <= 0) animSt = 1;  // Dog погоня = БЕГ a1 (ZT draw 0x19934), не ходьба a0
                    // НАПРАВЛЕНИЕ: climb Hydaca = DIRECTIONAL по ракурсу к игроку (6 поз, ZT draw states 3-A по self↔cam); иначе по скорости
                    uint8_t dir;
                    bool hydClimb = (animSt == 4 && a.srcType == 0x65 && A.climbDirs > 0);
                    if (animSt == 5) dir = (uint8_t)((a.hp < 0) ? 2 : (a.vz > 0.0 ? 0 : 1));  // ПАДЕНИЕ: 0=вверх(a8) 1=вниз(a1) 2=мёртв(a10) (ZT 15402 по $2e/$36)
                    else if (hydClimb) dir = (uint8_t)hydClimbPose(a.state, a.x - cam.px, a.y - cam.py);  // ЛАЗАНИЕ: per-state геометрия (ZT 1539e 3-A), НЕ atan2
                    else dir = (uint8_t)enemyDirIndex(a.vx, a.vy, cam.px - a.x, cam.py - a.y, A.walkDirs);  // поворот по углу
                    int nf = (animSt == 1) ? (int)A.fire.size() : (animSt == 2) ? (int)A.hit.size()
                                           : (animSt == 4) ? (int)(hydClimb ? A.climbDir[dir].size() : A.climb.size())
                                           : (animSt == 5) ? (int)((dir==0) ? A.fallUp.size() : (dir==2) ? A.fallDead.size() : A.climb.size())
                                           : (int)A.walk[dir < A.walkDirs ? dir : 0].size();
                    if (nf < 1) nf = 1;
                    bool dogRun = (a.srcType == 0x68 && animSt == 1 && a.fireAnimT <= 0);           // Dog бег: цикл по движению, не по fire-таймеру
                    uint8_t efr = dogRun            ? (nf > 1 ? (uint8_t)((a.frameT / 6) % nf) : 0)
                                : (animSt == 1) ? (uint8_t)(((12 - a.fireAnimT) / 3) % nf)   // стрельба: прогон кадров
                                : (animSt == 2) ? (uint8_t)((a.hitT / 3) % nf)                // стаггер (hitT растёт)
                                : (nf > 1 ? (uint8_t)((a.frameT / 6) % nf) : 0);              // ходьба: по движению
                    fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, efr, false, (float)a.z, dir, animSt, a.variant});
                } else
                    fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)-8, 16, 7});
                break; }
            case AT_CORPSE: {                             // труп = CORPSE-anim (ZT corpse-draw); кадр по типу врага
                int slot = enemyGfxSlot(a.srcType);
                if (slot >= 0 && slot < 16 && g_enemyAnim[slot].ok) {
                    EnemyAnimSet& A = (a.variant && g_enemyAnimVar2[slot].ok) ? g_enemyAnimVar2[slot] : g_enemyAnim[slot];
                    bool hasDeath = !A.death.empty();
                    int nd = hasDeath ? (int)A.death.size() : 1;
                    // КАДР ТРУПА (ZT corpse-draw d0): FH мерцает LUT@0x186a8 (0→2→4→2, «дёргается»); Hydaca RNG-мерцание
                    // (ножки вверх-вниз); Imp/FH-SF кадр 0 (или 1 при простреле, $35=a.state); прочие — статик кадр 0
                    // (ZT d2=−1 «осел» / 1-кадровые). Простреливание (corpse-hHit) крутит a.state → сменяет кадр.
                    uint8_t efr;
                    if (slot == 1 && nd > 1) {                        // FH: LUT-мерцание + фаза простреливания
                        static const uint8_t FHLUT[16] = {0,0,0,0,2,2,2,2,4,4,4,4,2,2,2,2};
                        uint8_t f = FHLUT[(a.frameT >> 1) & 0xf]; if (f >= nd) f = (uint8_t)(nd - 1);
                        efr = (uint8_t)((f + (a.state & 1)) % nd);
                    } else if (slot == 3 && nd > 1) {                 // Hydaca (corpse-draw 0x15618): d2=rnd + (state2?2) + (bit$35?4)
                        // ДВА состояния: обычный труп = кадры 0/1 (лёгкое мерцание «ножки»); ПРОСТРЕЛЕННЫЙ (после corpse-hHit,
                        // a.state≥1) = кадр 6 (раскрытый, кишки). Мерцание МЕДЛЕННОЕ — раз в ~4 кадра (ZT draw реже 60fps),
                        // а не бешеный RNG каждый кадр.
                        int tick = a.frameT >> 2;                    // игровой тик ≈ каждые 4 кадра
                        uint32_t r = (uint32_t)(tick * 1103515245u + 12345u);
                        int lo = (a.state >= 1) ? 6 : 0;             // простреленный → кадр 6; обычный → 0/1
                        efr = (uint8_t)(lo + ((r >> 20) & 1));
                        if (efr >= nd) efr = (uint8_t)(nd - 1);       // clamp (кадр 7 → 6)
                    } else if ((slot == 2 || slot == 7) && nd > 1) {  // Imp/FH-SF: кадр 0 (при простреле → 1)
                        efr = (uint8_t)((a.state & 1) && nd > 1 ? 1 : 0);
                    } else {                                          // статичный труп: кадр 0 (не последний!)
                        efr = (uint8_t)((a.state & 1) && nd > 1 ? 1 : 0);   // 1-кадровые → всегда 0; многокадр. Sgt/Reven → 0/1
                    }
                    fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, efr, !hasDeath, (float)a.z, 0,
                                  (uint8_t)(hasDeath ? 3 : 0), a.variant, false, a.burned});   // burned → обугленный труп
                }
                // ОБРОНЕННОЕ ОРУЖИЕ = ВИДИМЫЙ пикап на полу у трупа (ZT death-think: cmd 0x13 → объект-пикап в 0x1c03a,
                // НЕ бросок-анимация). Тайл объект-банка по item-id (как itemBillboardForCt). Подбор шагом (rcTryCorpsePickup).
                if (a.drop >= 1 && a.drop < 15) {
                    static const uint8_t ITILE[15] = {0,3,8,10,11,15,4,13,14,7,5,6,12,18,19};   // item-id → объект-банк тайл
                    fx.push_back({a.x, a.y, a.floor, ITILE[a.drop], (int8_t)4, 4, 4});           // ~S/4, низом на полу
                }
                break; }
            default: break;
        }
    }
}
