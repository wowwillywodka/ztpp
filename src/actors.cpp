// ztpp — src/actors.cpp: гигантский per-frame AI/update-цикл updateActors (вынесен из actors.hpp).
// Не-шаблон, не per-pixel (раз в кадр) → компилируется один раз, не в каждой TU. Прочая логика — в actors.hpp.
#include "actors.hpp"
#include "sniper_overlay.hpp"   // фоновый снайпер: ракета за край крыши → трассер 28b4

void updateActors(const Level& lvl, const Camera& cam) {
    auto& v = actors();
    // ⭐ЭВИКЦИЯ-ТИК (ROM 16094: сервис зовётся каждый 8-й кадр на актёра, стаггер по номеру слота):
    // актёры живут только «пузырём» вокруг игрока — дальние сериализуются из пула (враг→маркер, труп→статик)
    static uint32_t s_evTick = 0; ++s_evTick;
    // порог: ROM = жёстко 0xA00 (10 кл, октаг., 2D — ЭТАЖ НЕ УЧАСТВУЕТ); пробуждение статуи-маркера
    // капнуто 8 кл (updateEnemySpawns) → гистерезис 8/10 ROM-точный, кольца спавн/эвикт нет.
    const double evictDist = 10.0;
    openDoorsAtEnemies(lvl, cam.floor);   // ZT b35c: дверь держится ОТКРЫТОЙ, пока в её клетке актёр (bit4)
    updatePlayerStepOnFire(cam);          // ZT e662: урон огнём при ВХОДЕ в клетку карта-огня (step-on, не радиус)
    // скорость игрока (для УПРЕЖДЕНИЯ точки прицела врага, как ZT добавляет target.vx/vy): дельта позы за кадр
    static double s_ppx = cam.px, s_ppy = cam.py;
    double pvx = cam.px - s_ppx, pvy = cam.py - s_ppy; s_ppx = cam.px; s_ppy = cam.py;
    { PlayerState& p = player();   // flashCram затухает в main по VBlank-правилу 0xB12 (не в сим-тике); i-frames СНЯТЫ (ROM d800 их не имеет)
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
                // ⭐РАКЕТА ЗА КРАЙ КАРТЫ над крышей (ROM 14076 @1409c: X+vx ≥ 0x1F00 и ТЕКУЩАЯ клетка
                // 0x27/0x28 → jsr 28b4(#$120) + free БЕЗ взрыва) — снаряд «улетает на фоновый слой»:
                // трассер 28f6 летит к снайперу; при |X_снайпера−центр|<0x40 наведение и убийство НАВСЕГДА.
                // (ROM для 0x27 требует Z снаряда >0 — порт высоту ракеты не моделирует, гейт опущен.)
                if (hit && nx >= 31.0 && faSnipers()) {
                    int ocx = (int)a.x, ocy = (int)a.y;
                    uint8_t oct = (ocx >= 0 && ocy >= 0 && ocx < Level::W && ocy < Level::H)
                                  ? lvl.cellType(a.floor, ocx, ocy) : 0;
                    if (oct == 0x27 || oct == 0x28) { snip::aimTrigger(0x120); a.active = false; break; }
                }
                // ⭐РАКЕТА В «НЕБЕСНУЮ» СТЕНУ (ROM 140f4→13cbc): парапет крыши / окно станции (по сырому
                // cellID) → снаряд исчезает БЕЗ взрыва — «улетел в небо». ROM берёт high по Z снаряда $24
                // (ракета наследует питч стрелка) — порт высоту ракеты не моделирует, high=true всегда
                // (отступление: из приседа ракета о парапет в ROM взорвалась бы).
                if (hit && cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H &&
                    cellBlockedAt(lvl.cellType(a.floor, cx, cy), nx - cx, ny - cy) &&
                    bulletSkyCellId(lvl, lvl.cellId(a.floor, cx, cy), true)) { a.active = false; break; }
                // РАКЕТА В ВРАГА (ZT снаряд 15f6e: актёр в ≤0x40≈0.25кл → детонация) — иначе пролетала насквозь.
                // НЕ снапим в позицию врага (иначе спрайт взрыва ложится ПОВЕРХ врага) — взрыв на месте ракеты, радиус достанет.
                if (!hit) for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                                gameDist(e.x - nx, e.y - ny) < 0.5) { hit = true; break; }
                if (hit) {                                // взрыв ЧУТЬ ПЕРЕД стеной (иначе z-режется стеной)
                    double sp = std::hypot(a.vx, a.vy);
                    if (sp > 0 && (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                                   cellBlockedAt(lvl.cellType(a.floor, cx, cy), nx - cx, ny - cy))) {
                        // ⭐ИМПАКТ РАКЕТЫ ломает СЕКРЕТ-СТЕНУ в клетке удара (ROM 14114: a6bc по клетке)
                        if (cx >= 0 && cy >= 0 && cx < Level::W && cy < Level::H &&
                            wallIsSecret(lvl.cellType(a.floor, cx, cy))) requestDestruct(a.floor, cx, cy);
                        a.x -= a.vx / sp * 0.4; a.y -= a.vy / sp * 0.4;
                    }
                    a.active = false; explodeAt(lvl, a.x, a.y, a.floor, cam);   // взрыв ракеты (blast — на 2-м тике жизни взрыва)
                } else { a.x = nx; a.y = ny; if (--a.timer <= 0) { a.active = false; explodeAt(lvl, a.x, a.y, a.floor, cam); } }
                break;
            }
            case AT_GRENADE: {                           // ФИЗИКА (0x13adc): дуга+гравитация+отскок, фитиль → взрыв
                // ⭐ТОЛЬКО ФИТИЛЬ (ROM 13adc: proximity-взрывателя НЕТ — баллистика «ложится на цель» расчётом
                // фитиля при броске: с автонаведением фитиль = dist/64+8, без цели = 0x32). Прокс-детект убран.
                if (--a.timer <= 0) {
                    // ⭐ФИТИЛЬ-ВЗРЫВ ломает СЕКРЕТ-СТЕНЫ 3×3 (ROM 13bf8: клетки ±0x100 → a6bc). Цепной/прострел (13dcc) — НЕ ломает.
                    int gx = (int)a.x, gy = (int)a.y;
                    for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) {
                        int wx = gx + ox, wy = gy + oy;
                        if (wx >= 0 && wy >= 0 && wx < Level::W && wy < Level::H &&
                            wallIsSecret(lvl.cellType(a.floor, wx, wy))) requestDestruct(a.floor, wx, wy);
                    }
                    explodeAt(lvl, a.x, a.y, a.floor, cam, a.z); a.active = false; break;   // взрыв на ВЫСОТЕ гранаты (z)
                }
                double nx = a.x + a.vx, ny = a.y + a.vy;   // горизонталь + отскок от стен (reflect+damp, как neg/asr)
                bool bounced = false;
                // ⭐СТЕНА для гранаты = ROM 13d42: класс-1 стена ИЛИ ЗАКРЫТАЯ дверь (класс 6/7 → non-zero → блок).
                // При открытии двери ROM переписывает клетку проходимым celltype (b1c4), поэтому открытую дверь
                // граната пролетает; закрытую — отскок. Порог открытия 0.4 (как LOS/Revenant, doorOpen 0..1).
                auto gBlk = [&](int cx, int cy) { uint8_t gc = lvl.cellType(a.floor, cx, cy);
                    return cellBlocks(gc) || (cellIsDoor(gc) && doorOpen(a.floor, cx, cy) < 0.4); };
                if (gBlk((int)nx, (int)a.y)) { a.vx = -a.vx * 0.5; nx = a.x; bounced = true; }
                if (gBlk((int)a.x, (int)ny)) { a.vy = -a.vy * 0.5; ny = a.y; bounced = true; }
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
                    // ⭐УРОН ИМПАКТА (ZT 14300): AoE радиус 0x100=1кл + LOS, урон = 0x400−dist (ZT 15732: d0=0x400−dist, HP−=d0).
                    //   Вблизи ~0x400 (почти макс), к краю ~0x300. РАЗОВЫЙ на импакт (не стрим). Было фикс.урон 2 = слишком мало.
                    for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor) {
                        double dd = std::hypot(e.x - a.x, e.y - a.y);
                        if (dd < 1.0 && enemyLOS(lvl, a.floor, a.x, a.y, e.x, e.y)) {
                            int raw = 1024 - (int)(dd * 256.0); if (raw < 1) raw = 1;    // 0x400 − dist_units
                            // ⭐СПАЛЁН ОГНЁМ (ZT: receive пишет +0x30 из огонь-флага −$5910; death читает +0x30 → 1b8d8 BURNT REMAINS).
                            //   ГОРЮЧИЕ (органика): Sgt 0x29/FH 0x2A/Imp 0x2B/Hydaca 0x65/Dog 0x68/FH-SF 0x69 (death-путь 1b8d8: 1b4a0/184f2/
                            //   189f2/1565a/19808/19e50). НЕ горят: боссы + Revenant 0x66 (робот). Труп → burnt-remains СПРАЙТ, не перекраска.
                            switch (e.srcType) { case 0x29: case 0x2A: case 0x2B: case 0x65: case 0x68: case 0x69: e.burned = true; break; }
                            hitEnemy(e, raw / 100 < 1 ? 1 : raw / 100, a.x, a.y, raw);
                        }
                    }
                    int fx0 = (int)a.x, fy0 = (int)a.y;
                    if (fx0 >= 0 && fy0 >= 0 && fx0 < Level::W && fy0 < Level::H && !cellBlocks(lvl.cellType(a.floor, fx0, fy0)))
                        spawnFire(a.x, a.y, a.floor, A_FIRE_LIFE, true);              // НАЗЕМНЫЙ ОГОНЬ = ВИЗУАЛ 16 тиков (ZT 1423a: урона НЕТ, playerSafe)
                    a.active = false; break;
                }
                a.x = nx; a.y = ny;                                                  // в полёте НЕ жжёт (урон — только импакт/наземный огонь)
                break;
            }
            case AT_FIRE: {                              // ОГОНЬ-ОБЪЕКТ (ZT 0x142c6): вечный(карта)/фитиль(огнемёт). ВРАГОВ НЕ жжёт,
                // ИГРОКА жжёт по STEP-ON (updatePlayerStepOnFire, вызов раз/кадр — ZT e662 через e3f8 при СМЕНЕ клетки, НЕ per-frame).
                ++a.frameT;
                if (a.fireCd > 0) { if (--a.fireCd <= 0) a.active = false; break; }  // ЗАТУХАНИЕ от пены: гаснет
                if (a.timer >= 0 && --a.timer <= 0) a.active = false;          // фитиль (огнемёт); карта (timer<0) — вечно
                break;
            }
            case AT_FOAM: {                              // ПЕНА огнетушителя: ПАДАЕТ вниз, ТУШИТ огонь, БЕЗ урона
                for (int fp = 0; fp < 2; ++fp)           // огонь: транзиент в пуле + карта-огни в статик-мире
                 for (auto& e : (fp ? staticActors() : v))
                  if (e.active && (e.think == AT_FIRE || e.think == AT_FLAME) && e.floor == a.floor &&
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
            case AT_EXPLOSION:                            // ⭐BLAST на 2-м тике жизни (ROM 14196: $1e==2 → 16294)
                if (++a.timer == 2 && a.state == 1) { a.state = 0; blastAt(lvl, a.x, a.y, a.floor, cam); }
                if (a.timer >= A_EXPL_FRAMES) a.active = false; break;
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
                    // ⭐ДИАГОНАЛЬ (ct 0x02-0x05) = ТОНКАЯ ГИПОТЕНУЗА (cellBlockedAt полуплоскость по субкоордам, как ZT think 13d42→13cbc
                    //   и импакт пули traceMiss) — кровь пролетает пустую половину, липнет к грани, а НЕ к квадрату. Полные стены — идентично.
                    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                        cellBlockedAt(lvl.cellType(a.floor, cx, cy), nx - cx, ny - cy)) { hitWall = true; break; }
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
                const bool offFloor = (a.floor != cam.floor);
                if (!offFloor && a.gunBurst > 0) {          // ОЧЕРЕДЬ выстрела: 0x68(FM)+0x97(PSG-шум) ×3 через ~120мс (MAME 0.1-0.15с). По времени — think на переменном FPS
                    // ⭐кадровый счётчик (был SDL_GetTicks — привязка к реальному времени): интервал 2 ROM-тика ≈133мс
                    if (a.gunBurstNext == 0) { snd::playSfx(0x68); snd::playSfx(0x97); --a.gunBurst;
                        a.gunBurstNext = (uint32_t)ticks(2); }
                    else --a.gunBurstNext;
                }
                // Sgt → FH-SF: трансформация по истечении таймера $41=50..81 тиков (ZT 1b18a: rnd&0x1f+0x32).
                // ⭐МОРФ — НЕ мгновенный (ZT state6 1b51a: $35++ до 9 = 9 тиков морф-фазы, актёр СТОИТ НА МЕСТЕ,
                // затем подмена think/draw/гфх + $41=0x1e). state=98 = морф-фаза порта.
                if (a.srcType == 0x29 && a.state == 98) {
                    a.vx = a.vy = 0;                                      // замер на месте (ZT: state6 не двигается)
                    if (--a.timer <= 0) {
                        a.srcType = 0x69; a.tile = enemyTileForCt(0x69);  // стал Former Human SF (1b528-1b55a)
                        a.state = 0; a.timer = 0; a.fireCd = ticks(30);   // ROM: после морфа $41=0x1e=30
                    }
                    break;
                }
                if (a.xformT > 0 && !enemiesFrozen()) {
                    if (--a.xformT == 0 && a.srcType == 0x29) { a.state = 98; a.timer = ticks(9); break; }   // → морф-фаза 9 тиков
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
                    // ⭐REVENANT: стаггер = ФИКС 10 тиков (ZT 1afc6: state 0xA, $35=0xA, декремент 1/тик;
                    // НЕ по затуханию скорости) — иначе кадры падения (draw 1adfe) не успевают проиграться.
                    bool revFix = (a.srcType == 0x66);
                    double sp = std::hypot(a.vx, a.vy);
                    if (revFix ? (a.hitT < ticks(10)) : (sp >= 0.039)) { // ещё «летит»/тикает → скользим + ÷2, держимся в стаггере
                        double nx = a.x + a.vx, ny = a.y + a.vy;
                        if (!enemyBlockedAt(lvl.cellType(a.floor, (int)nx, (int)a.y), a.floor, (int)nx, (int)a.y, nx - (int)nx, a.y - (int)a.y)) a.x = nx;
                        if (!enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ny), a.floor, (int)a.x, (int)ny, a.x - (int)a.x, ny - (int)ny)) a.y = ny;
                        enemyPlayerStandoff(a, cam, lvl);                     // анти-оверлап и в стаггере (не слиться с игроком)
                        a.vx *= 0.5; a.vy *= 0.5;                        // ZT: asr.w (÷2) скорости каждый кадр
                        ++a.hitT;                                        // кадр анимации стаггера (растёт пока держится)
                        break;
                    }
                    // скорость СЕЛА (ZT $188b2): решаем смерть / восстановление
                    if (a.hp <= 0) {                                     // HP<0 → СМЕРТЬ → труп на месте (ZT $189e4, v уже мала)
                        int dr = enemyWeaponDrop(a.srcType);            // солдаты роняют ствол (невидимо; подбор шагом на труп)
                        if (dr == -2) dr = (enemyRng() & 0x10) ? 10 : 7;  // Sergeant/FH-SF: btst #4 → laser(10) иначе grenade(7) (дизасм 0x1b716)
                        if (a.burned) dr = -1;                           // ⭐СОЖЖЁННЫЙ труп оружие НЕ отдаёт (ROM 184f2: tst $30 → 1b8d8, обугленный think БЕЗ state3/выдачи)
                        if (a.srcType == 0x67 || a.srcType == 0x6A || a.srcType == 0x6B)
                            episodeEndT() = ticks(15);                   // ⭐БОСС УБИТ → конец эпизода (ROM 18e5c/1933c/1a696: -$58dc=0xF, ~1с)
                        spawnCorpse(a.x, a.y, a.floor, a.srcType, a.vx, a.vy, a.variant, dr, a.burned);  // burned → обугленный труп
                        a.active = false; break;
                    }
                    a.hitT = 0; a.vx = a.vy = 0;                         // ВЫЖИЛ → восстановление к обычному AI (ZT $34=0)
                    break;                                              // пауза кадр на восстановление
                }
                if (enemiesFrozen()) break;                              // чит: враги замерли
                // ⭐ЭВИКЦИЯ ВРАГА (ROM 16094→14a56, objdef+0xC): каждый 8-й тик — если 2D-октаг. дистанция до игрока
                // ≥ порога, враг «пере-засыпает»: обратно в pendingSpawns СВЕЖИМ маркером в текущей клетке
                // (HP/морф теряются — ROM-верно, маркер состояния не несёт), слот освобождается. Боссы не
                // эвиктятся (фазы/притворство Boss3 нельзя терять; ROM-люфт: они и не уходят от игрока далеко).
                if (((s_evTick + (uint32_t)(&a - v.data())) & 7) == 0 && a.hitT == 0 && a.hp > 0 &&
                    a.srcType != 0x67 && a.srcType != 0x6A && a.srcType != 0x6B &&
                    gameDist(a.x - cam.px, a.y - cam.py) >= evictDist) {
                    int mx = (int)a.x, my = (int)a.y;
                    if (mx >= 0 && my >= 0 && mx < Level::W && my < Level::H) {
                        pendingSpawns().push_back({mx, my, a.floor, a.srcType});
                        a.active = false;
                    }
                    break;
                }
                // ⭐ОФФ-ЭТАЖНАЯ СИМУЛЯЦИЯ (BACKLOG; ROM 13232: диспетчер think БЕЗ гейта по этажу — разбуженные
                // идут к (x,y) игрока и «встречают у лестницы», таймеры тикают). Порт-аппроксимация: движение
                // к (px,py) игрока ROM-скоростью с клеточной коллизией (двери виртуально открывает: canOpen)
                // + advance/окно-цикл продолжает крутиться; атак/уронов/звуков на чужом этаже НЕТ.
                // (Морф Sgt (state98/xformT) и притворство Boss3 (state99) тикают ВЫШЕ — общие блоки.)
                if (offFloor) {
                    EClass oec = enemyClass(a.srcType);
                    bool engaged = (oec == EC_RANGED_FAST) ? (a.state >= 8) : (a.state >= 1);  // Revenant-патруль/спящие НЕ стягиваются
                    if (!engaged) { a.vx = a.vy = 0; break; }
                    EMove omv = enemyMove(a.srcType);
                    double ovx = (cam.px - a.x) * omv.gain, ovy = (cam.py - a.y) * omv.gain;
                    if (ovx >  omv.vmax) ovx =  omv.vmax; else if (ovx < -omv.vmax) ovx = -omv.vmax;
                    if (ovy >  omv.vmax) ovy =  omv.vmax; else if (ovy < -omv.vmax) ovy = -omv.vmax;
                    double nx = a.x + ovx, ny = a.y + ovy;
                    if (!enemyBlockedAt(lvl.cellType(a.floor, (int)nx, (int)a.y), a.floor, (int)nx, (int)a.y, nx - (int)nx, a.y - (int)a.y, true)) a.x = nx;
                    if (!enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ny), a.floor, (int)a.x, (int)ny, a.x - (int)a.x, ny - (int)ny, true)) a.y = ny;
                    a.vx = ovx; a.vy = ovy;                              // для walk-анимации по возвращении
                    if (a.fireCd > 0) --a.fireCd;
                    if (--a.timer <= 0) {                                // advance↔окно крутятся (фаза жива к приходу игрока)
                        a.state = (a.state == 1) ? 2 : 1;
                        a.timer = (a.state == 2) ? ticks(10) : ticks(12 + (int)(enemyRng() & 15));
                    }
                    break;
                }
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
                    // ⭐КОЛЛИЗИЯ = ТОЧНАЯ ROM-МОДЕЛЬ 145aa/146fe (2026-07-21, чинит «Hydaca не протискивается между
                    // диагональю и стеной» — юзер, ENGINEERING 1): ROM двигает ЦЕНТР (клетка назначения проверяется
                    // по центру!), а от стен держит не edge-probe, а КЛАМП СУБ-ПОЗИЦИИ $21/$23 к [0x40,0xbf] ТОЛЬКО
                    // при солидном соседе с той стороны. В проходе шириной 1 клетка (диагональ-квадрат слева + стена
                    // справа) центр идёт серединой клетки — кламп сжимает к [0.25,0.746], проход ПРОХОДИМ.
                    // Прежний edge-probe (±0.26 по ходу) блокировал такие проходы намертво (край задевал соседей).
                    const bool canOpen = enemyOpensDoors(a.srcType);    // только 5 типов толкают двери (ZT b1c4/b202)
                    // САМОСПАСЕНИЕ: центр уже в solid-клетке → блокировку снимаем, враг выходит свободным движением
                    const bool stuck = enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)a.y), a.floor, (int)a.x, (int)a.y, a.x - (int)a.x, a.y - (int)a.y, false);
                    double nx = a.x + a.vx, ny = a.y + a.vy;
                    if (a.vx != 0 && (stuck || !enemyBlockedAt(lvl.cellType(a.floor, (int)nx, (int)a.y), a.floor, (int)nx, (int)a.y, nx - std::floor(nx), a.y - (int)a.y, canOpen))) a.x = nx;
                    if (a.vy != 0 && (stuck || !enemyBlockedAt(lvl.cellType(a.floor, (int)a.x, (int)ny), a.floor, (int)a.x, (int)ny, a.x - (int)a.x, ny - std::floor(ny), canOpen))) a.y = ny;
                    if (!stuck) {                                       // ROM 145aa: кламп суб-позиции у солидных соседей
                        int cx = (int)a.x, cy = (int)a.y;
                        auto solid = [&](int sx, int sy) {
                            if (sx < 0 || sy < 0 || sx >= Level::W || sy >= Level::H) return true;
                            // ⚠canOpen ОБЯЗАТЕЛЕН (регрессия 2026-07-21: кламп держал ОПЕНЕРОВ в 0.25 от двери —
                            // центр не входил в дверную клетку → openDoorsAtEnemies не срабатывал → двери «сломались»)
                            return enemyBlockedAt(lvl.cellType(a.floor, sx, sy), a.floor, sx, sy, 0.5, 0.5, canOpen); };
                        double sx = a.x - cx, sy = a.y - cy;
                        const double LO = 0x40 / 256.0, HI = 0xBF / 256.0;
                        if (sx < LO && solid(cx - 1, cy)) a.x = cx + LO;
                        else if (sx > HI && solid(cx + 1, cy)) a.x = cx + HI;
                        if (sy < LO && solid(cx, cy - 1)) a.y = cy + LO;
                        else if (sy > HI && solid(cx, cy + 1)) a.y = cy + HI;
                    }
                    enemyPlayerStandoff(a, cam, lvl);                         // АНТИ-ОВЕРЛАП (ZT 0x146fe): не проваливаться в игрока
                    if (std::hypot(a.vx, a.vy) > 0.004) a.frameT += 2;   // кадр ходьбы (anim0) при движении
                };

                // ═══ DOG (0x19540): НАСКОК-ОТХОД (hit-and-run). ⭐ПЕРЕПИСАНО 2026-07-15 по дизасму (было «липнет к игроку+кусает
                //   циклично»). ROM: патруль-ДРЕЙФ (текущая поз ±2кл) → ДЕТЕКТ dist<0x300=3кл БЕЗ LOS («чует сквозь стену», 1974e) →
                //   ПОГОНЯ (быстро к игроку, трек раз в 4 тика) → таймер/касание → ОДИН укус (dist<0x80) + ОТХОД к точке СТАРТА
                //   ($48/$4a=поз при детекте) → достиг старта → новый патруль. homeX/homeY = точка старта наскока. ═══
                if (ec == EC_DOG) {
                    double dcx = std::hypot(a.aimX - a.x, a.aimY - a.y);          // дист до текущей цели движения ($38/$3a)
                    auto newWander = [&]{ a.aimX = a.x + ((int)(enemyRng() & 0x3ff) - 512) / 256.0;   // ZT 197c6: текущая поз ± (rnd&0x3ff−0x200)=±2кл
                                          a.aimY = a.y + ((int)(enemyRng() & 0x3ff) - 512) / 256.0; };
                    if (a.state == 0) {                                          // ПАТРУЛЬ-ДРЕЙФ (медленно, vmax ±0x14)
                        if (d < 3.0) {                                           // ДЕТЕКТ dist<0x300 БЕЗ LOS (ZT 19882)
                            a.state = 1; a.timer = ticks((int)(d * 4.0) + 6);   // $35=(dist>>6)+6 ×enemyTimerScale
                            a.homeX = a.x; a.homeY = a.y;                        // $48/$4a = точка старта наскока (поз при детекте)
                            a.aimX = cam.px; a.aimY = cam.py; break;
                        }
                        moveTo(a.aimX, a.aimY, 0.28);                            // дрейф (vmax ±0x14/±0x48)
                        if (dcx < 0.06 || (a.aimX == 0 && a.aimY == 0)) newWander();  // достиг точки → новая ±2кл
                        break;
                    }
                    if (a.state == 1) {                                          // ПОГОНЯ (быстро, трек игрока раз в 4 тика — ZT 19598)
                        if ((a.frameT & 3) == 0) { a.aimX = cam.px; a.aimY = cam.py; }
                        moveTo(a.aimX, a.aimY, 1.0);                             // ±0x48
                        a.atkPose = (d < 1.0 || a.timer < 6) ? 1 : 0;           // ⭐прыжок-поза a2 при близко/скоро; иначе бег a1 (ZT draw 198c4: $35<6||dist<1кл→a2)
                        if (--a.timer <= 0 || dcx < 0.06 || d < 0.5) {          // ДОСТИГ игрока (d<0.5 = контакт, ZT 19606 dist к цели≤0xa) ИЛИ таймер → укус СРАЗУ (без задержки)
                            a.state = 2; a.aimX = a.homeX; a.aimY = a.homeY;     // цель = точка СТАРТА (ZT 1961c: $38/$3a=$48/$4a)
                            if (d < 0.5) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; snd::playSfxPolite(0x1b, 0x0F);
                                           player().knockVx *= 2.0; player().knockVy *= 2.0; }  // ОДИН укус dist<0x80 (ZT 19678, урон 0x28a→6, звук 0x1b) + ОТБРОС ×2 (196ae: asl скоростей игрока)
                            a.timer = ticks(20);                                 // $35=0x14 ×enemyTimerScale
                        }
                        break;
                    }
                    moveTo(a.aimX, a.aimY, 1.0);                                 // state2 ОТХОД к точке старта (±0x48)
                    if (dcx < 0.06 || --a.timer <= 0) { a.state = 0; newWander(); }   // достиг старта → патруль
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
                        // ⭐КЛАМП-МАРЖА (ZT 152c8, fall-through КАЖДОГО шага 15246): суб-позиция клампится к 0x20/0xdf,
                        // если соседняя клетка блокирована — Hydaca НЕ вкапывается в стену, порог климба ловится на клампе.
                        int cx0 = (int)std::floor(a.x), cy0 = (int)std::floor(a.y);
                        double fx = a.x - cx0, fy = a.y - cy0;
                        const double LO = 0x20 / 256.0, HI = 0xDF / 256.0;
                        auto blockedN = [&](int bx, int by){ return bx < 0 || by < 0 || bx >= Level::W || by >= Level::H ||
                            enemyBlockedAt(lvl.cellType(a.floor, bx, by), a.floor, bx, by, 0.5, 0.5, false); };
                        if (fx < LO && blockedN(cx0 - 1, cy0)) a.x = cx0 + LO;
                        else if (fx > HI && blockedN(cx0 + 1, cy0)) a.x = cx0 + HI;
                        if (fy < LO && blockedN(cx0, cy0 - 1)) a.y = cy0 + LO;
                        else if (fy > HI && blockedN(cx0, cy0 + 1)) a.y = cy0 + HI;
                    };
                    // КРАУЛ к aim БЕЗ R-марджина moveTo (иначе Hydaca стоит в 0.26кл от стены и НЕ доходит до края для климба;
                    // ZT move 15246 — вплотную). Скорость как enemyMove, коллизия по клетке → достаёт до грани стены.
                    auto hydCrawl = [&](double scale){
                        double vmx = mv.vmax * scale;
                        double avx = (a.aimX - a.x) * mv.gain, avy = (a.aimY - a.y) * mv.gain;
                        if (avx >  vmx) avx =  vmx; else if (avx < -vmx) avx = -vmx;
                        if (avy >  vmx) avy =  vmx; else if (avy < -vmx) avy = -vmx;
                        a.vx = avx; a.vy = avy;                                   // ⭐скорость для directional-анимации пола (enemyDirIndex) — иначе dir всегда 0=фронт
                        hydStep(avx, avy); enemyPlayerStandoff(a, cam, lvl);
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
                        if (a.vz > 0 && a.z >= 1.0) { a.z = 1.0; a.variant = 1; a.state = 1; a.vz = 0; hydReaim(); }        // долез до ПОТОЛКА → st1 (ZT 15052: $35=0 → re-aim, БЕЗ кулдауна)
                        else if (a.vz < 0 && a.z <= 0.0) { a.z = 0.0; a.variant = 0; a.state = 1; a.vz = 0; hydReaim(); }   // спустился на ПОЛ → st1 (ZT 1507a)
                        a.frameT += 2; break;
                    }
                    // ═══ ПАДЕНИЕ (a.vz != 0, ZT state2 0x14ca6): гравитация + ЛАТЕРАЛЬ (15246 move + ½-декей) ═══
                    if (a.vz != 0.0) {
                        a.z += a.vz; a.vz -= 3.0 / 48.0;                 // гравитация (ZT 0x14ca6: $2e−=3)
                        hydStep(a.vx, a.vy); a.vx *= 0.5; a.vy *= 0.5;   // ЛАТЕРАЛЬ вдоль вектора срыва + затухание ×½ (ZT 14cdc move+asr)
                        if (a.z <= 0.0) {                               // ПРИЗЕМЛЕНИЕ на пол (ZT 14cc2-14cd8)
                            a.z = 0.0; a.vz = 0.0; a.variant = 0;
                            if (a.hp <= 0) {                            // сбита насмерть → труп (звук 0x25 играет spawnCorpse через enemyDeathSfx — НЕ дублировать)
                                spawnCorpse(a.x, a.y, a.floor, a.srcType, a.vx, a.vy, a.variant, -1, a.burned); a.active = false; break; }  // остаточная латераль → короткое скольжение трупа
                            hydReaim();                                 // ⭐ROM: bra 14bb4 = НЕМЕДЛЕННЫЙ re-aim (не таймер 60+rnd — ползла в устаревшую точку)
                        }
                        a.frameT += 2; break;
                    }
                    // ═══ КРАУЛ (state 1): вход в климб у стены (1/8/тик) → wander-краул + укус (пол) / отцеп (потолок) ═══
                    int wallDir = hydacaWallDir(lvl, a.floor, a.x, a.y);      // стена вплотную (0=W,1=E,2=N,3=S); −1 нет
                    // ВХОД В КЛИМБ (ZT 150de/клоны): у стены + 1/8 rng → climb-стейт по (сторона стены, латераль к aim), z-vel rnd(2..5).
                    auto tryClimb = [&]() -> bool {
                        if (wallDir < 0 || (enemyRng() & 0xe0) != 0) return false;   // 1/8 шанс/тик (ZT rng&0xe0==0); БЕЗ fireCd-гейта (в ROM нет)
                        // ⭐АНТИ-УГОЛ (ZT 14d8a/14d92 и клоны): frac ВДОЛЬ стены обязан быть в СРЕДНЕЙ половине клетки
                        // [0x40..0xbf] — в углу (обе frac у края) ни одна ветка не проходит → ROM в углах НЕ лезет (нет петли).
                        { double fx = a.x - std::floor(a.x), fy = a.y - std::floor(a.y);
                          double perp = (wallDir <= 1) ? fy : fx;                    // W/E стена → полоса по Y; N/S → по X
                          if (perp < 0x40 / 256.0 || perp > 0xBF / 256.0) return false; }
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
                        // ⭐ОТЦЕП ДО движения (ZT 14d38-14d46: state=2 + clr vx/vy/vz, rts — падение строго ВНИЗ в этот тик)
                        if ((enemyRng() & 0x7f) == 0) { a.vx = a.vy = 0; a.vz = -0.0001; a.frameT += 2; break; }
                        hydCrawl(1.0);                                   // потолочный краул = ТА ЖЕ скорость (ZT 14e0a общий код)
                        if (--a.timer <= 0) hydReaim();
                    } else {                                            // НА ПОЛУ
                        a.variant = 0;
                        hydCrawl(1.0);
                        if (d < 0.39 && a.fireCd <= 0) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireCd = ticks(10); a.fireAnimT = 10;
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
                        if (d < 0.25) { damagePlayer(2, cam.px, cam.py, a.x, a.y); snd::playSfxPolite(0x1b, 0x0F); }  // контакт ≤0x40 (ZT 0x1a9f6 d0=0x384→2) КАЖДЫЙ тик + звук 0x1b (ROM без инвулна — так и есть)
                        double da = std::hypot(a.aimX - a.x, a.aimY - a.y);
                        if (da < 0.15 || --a.timer <= 0) { a.state = 9; a.timer = 10; a.fireAnimT = 0; }  // достиг aim / таймер → ПРИЦЕЛ 10т ($35=0xa)
                    } else {                                              // state 9 AIM/FIRE (ZT 0x1aa28): движется к aim, ОДИН выстрел на tick6
                        int t = --a.timer;                                // 9,8,...,0
                        if (t == 6) {                                     // ZT $35==6 → ВЫСТРЕЛ (hitscan лазер)
                            a.fireAnimT = 12;
                            if (los) {
                                bool cm = enemyShotMiss(lvl.env(a.floor));    // стелс-ролл (стойка+тьма, ZT 0x1aa76 — как FH)
                                if (!cm && d < 2.0) { int rd = 16 - (int)(d * 8.0); if (rd < 1) rd = 1; damagePlayer(rd, cam.px, cam.py, a.x, a.y); }  // урон 16−(dist·2>>6), ≤2кл (ZT 0x1aac4)
                                snd::playSfxPolite(0x1e, 0);                       // лазер (hit ИЛИ miss, ZT 0x1aad8)
                            }
                        } else if (t == 5 || t == 4) snd::playSfxPolite(0x1e, 0);  // ZT $35==5/4 → заряд-звук 0x1e
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
                    // ⭐УПРЕЖДЕНИЕ per-enemy (2026-07-15 сверка; было унифицир. pvx*6 вперёд для ВСЕХ дальних/боссов = перелёт+неверный знак):
                    //   FH 0x2A: +1×vel вперёд (ZT 18388 add $2a). Sgt 0x29 / FH-SF 0x69: ПЕРПЕНДИКУЛЯР ½vel (перехват стрейфа,
                    //   ∓vy/2,±vx/2 знак по $3e&1; FH-SF +½vel вперёд) (ZT 1b0f4/19af4). Boss1 0x67 / Boss2 0x6B: −1×vel — АНТИ-упреждение,
                    //   целит ПОЗАДИ движения (ZT 190f4/018c6e sub $2a). Boss3 0x6A / Imp 0x2B: без упреждения (только rnd).
                    double lx = 0, ly = 0;
                    int sgn = (a.variant & 1) ? 1 : -1;                  // знак перпендикуляра по $3e&1 (btst#0)
                    switch (a.srcType) {
                        case 0x2A: lx = pvx; ly = pvy; break;                                       // FH: +1× вперёд
                        case 0x29: lx = -pvy * 0.5 * sgn; ly = pvx * 0.5 * sgn; break;              // Sgt: перпендикуляр ½
                        case 0x69: lx = -pvy * 0.5 * sgn + pvx * 0.5; ly = pvx * 0.5 * sgn + pvy * 0.5; break;  // FH-SF: перп ½ + ½ вперёд
                        case 0x67: case 0x6B: lx = -pvx; ly = -pvy; break;                          // Boss1/Boss2: −1× (анти-упреждение)
                        default: break;                                                            // Boss3/Imp: без упреждения
                    }
                    a.aimX = cam.px + lx + offx;
                    a.aimY = cam.py + ly + offy;
                    // advance-таймер ПО ДИСТАНЦИИ (ZT 0x18334 `$35 = dist>>6 + rnd`): чем ДАЛЬШЕ враг, тем дольше
                    // наступает между перецеливаниями; вблизи — чаще. dist>>6 = distКл·4; боссы dist>>7 = distКл·2.
                    // ⚠rnd-МАСКА РАЗНАЯ (2026-07-15 сверка): FH 0x2A / Sgt 0x29 = rnd&0x1f (0..31); Imp 0x2B (188c6) /
                    // FH-SF 0x69 (19ac8) / боссы = rnd&0xf (0..15). Было унифицировано 0x1f для всех не-боссов (Imp/FH-SF вдвое шире).
                    int base, rmax;
                    if (ec == EC_BOSS_MELEE || ec == EC_BOSS_PROJ) { base = (int)(d * 2.0); rmax = 16; }   // ZT dist>>7 + rnd&0xf
                    else { base = (int)(d * 4.0); rmax = (a.srcType == 0x2A || a.srcType == 0x29) ? 32 : 16; }   // FH/Sgt 0x1f; Imp/FH-SF 0xf
                    // ⭐БЕЗ минимума (был кламп ticks(6) — отсебятина): ROM $35=(dist>>6)+rnd, вблизи 0..15 —
                    // атаки чаще; ноль (1/16 в упор) → subq/beq-обёртка = 256 тиков «замер» (ROM-глюк, повторяем).
                    int t35 = base + (int)(enemyRng() % rmax);
                    a.state = 1; a.timer = ticks(t35 == 0 ? 256 : t35);   // ×enemyTimerScale
                };
                // ⭐ГУМАНОИДЫ/БОССЫ: ГЕЙТА ОБНАРУЖЕНИЯ В ROM НЕТ (2026-07-17, спавнер a08e: при спавне из грида
                // $38/$3a=позиция ИГРОКА, $6=$FF119C → охота с ПЕРВОГО тика; FH state0 183aa идёт к цели БЕЗ LOS).
                // LOS/стелс-пороги гейтят ТОЛЬКО выстрел (18452/1a626). Порт-гейт «los && d<8» держал врагов
                // столбами во время боя (напр. вокруг Boss3) — в оригинале все заспавненные сходятся к игроку.
                if (a.state == 0) reaim();                              // спавн/пробуждение → сразу преследование
                if (d > 14.0 && !los) { a.vx = a.vy = 0; break; }       // дальний след (аналог far-cull 0xA00): замер, не сброс
                // Revenant («робот», ZT 0x1ac68): ВДАЛИ (d>1.2кл = 0x12c) крадётся МЕДЛЕННО (шаг ±0x10), рывок ±0x40
                //   только ВБЛИЗИ → «не несётся издали, атакует только когда близко» (юзер verif. трейсом).
                double mscale = (ec == EC_RANGED_FAST && d > 1.2) ? 0.25 : 1.0;
                // ⭐IMP ВЫПАД-ТЕЛЕГРАФ (ZT 1887a/18992): state1(advance) → state2 = 10 тиков ЗАМЕР в позе выпада
                // (a2), удар ОДИН на суб-тике 5 если d<0x64=0.39кл (X=0x100 → 12 HP, звук 0x94) → re-aim.
                bool impLunge = (ec == EC_MELEE && a.state == 2);
                moveTo(a.aimX, a.aimY, mscale);                          // ⭐ПОБАЙТНО (18920/183aa): движение идёт И в окне атаки (Imp в выпаде ДВИЖЕТСЯ — 1887a state1 общий move)
                // ⭐ТЕЛЕГРАФ Imp (ROM draw 18aa0): окно = ЗАМАХ (a1, atkPose=1) → УДАР (a2, $35∈[4,6]) → ЗАМАХ.
                // fireAnimT=1 → в state0 поза гаснет сразу (ROM: кадр 0 немедленно). Суб-кадры — дистанс-ходьба (d2=−1).
                if (impLunge) {
                    a.fireAnimT = 1;
                    a.atkPose = (a.timer <= ticks(6) && a.timer >= ticks(4)) ? 0 : 1;
                }
                bool bossCls = (ec == EC_BOSS_PROJ || ec == EC_BOSS_MELEE);
                if (impLunge && a.timer == ticks(5) && d < 0.39) {       // удар на середине выпада (ZT $35==5)
                    damagePlayer(12, cam.px, cam.py, a.x, a.y); snd::playSfx(0x94);
                }
                if (--a.timer <= 0) {                                    // state1 advance → state2 fire-окно → reaim
                    if (a.state == 1) { a.state = 2; a.timer = ticks(10); }   // ⭐ОКНО ВСЕМ = 10 тиков (ROM $35=0xa у всех гуманоидов/боссов; было 50 не-боссам)
                    else reaim();
                }
                // ⭐ГУМАНОИДЫ FH/Sgt/FH-SF — СТРЕЛЬБА НА СУБ-ТИКАХ ОКНА (побайтный аудит 2026-07-21, ROM 18420-184e2):
                // t5 = выстрел (LOS-fail → НЕМЕДЛЕННЫЙ reaim — не палят в стену до конца окна!); ролл-промах ЗВУЧИТ;
                // d≥4кл — тихий; t4/t3 = звуки очереди 0x68+0x97 (18442). Период атаки = advance(dist·4+rnd)+10 =
                // ДИСТАНС-ЗАВИСИМЫЙ (вблизи чаще). Заменяет fireCd=55-фикс (то было осознанным упрощением — снято).
                if ((ec == EC_RANGED || ec == EC_GRENADIER) && a.state == 2) {
                    int t = a.timer;
                    if (t == ticks(5)) {
                        if (!los) reaim();                               // ZT 1846a: bne 18334
                        else {
                            bool cm = enemyShotMiss(lvl.env(a.floor));
                            int gmask = (a.srcType == 0x29) ? 7 : 15;    // шанс гранаты Sgt 1/8, FH-SF 1/16
                            if (ec == EC_GRENADIER && d >= 1.0 && (enemyRng() & gmask) == 0) {
                                spawnGrenade(a.x, a.y, a.floor, rx, ry, 1, 1.0, true); a.fireAnimT = 12; a.atkPose = 1; snd::playSfxPolite(0x1b, 0x0F);
                            } else if (d < 4.0) {
                                if (!cm) damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y);
                                a.fireAnimT = 12; a.atkPose = 0;
                                snd::playSfx(0x68); snd::playSfx(0x97);  // выстрел (и промах звучит — ZT 184ce)
                            }
                        }
                    } else if ((t == ticks(4) || t == ticks(3)) && !a.atkPose) {   // очередь: звуки 2-го/3-го выстрела (граната → без очереди, ROM state5)
                        snd::playSfx(0x68); snd::playSfx(0x97);
                    }
                }
                // ⭐АТАКА БОССОВ — в fire-окне (state2) на СУБ-ТИКАХ timer (ZT $35==6/5/4): Boss2 ЗАЛП до 3 hitscan; Boss3 ближний(t5)
                //   ИЛИ дальний-залп при enrage HP<20; Boss1 снаряд(t5). Вынесено из fireCd-развязки (боссы бьют СЕРИЕЙ за окно, DPS был ~½).
                if (bossCls && a.state == 2 && los) {
                    int t = a.timer;                                     // 10..0 (суб-тик fire-окна)
                    bool bmiss = enemyShotMiss(lvl.env(a.floor));
                    if (ec == EC_BOSS_PROJ) {                            // Boss1: ОДИН снаряд на $35==5 (ZT 19250)
                        if (t == 5 && d < 12.0) {
                            // ⭐РАЗБРОС КУРСА (аудит 2026-07-21, think 19060: rnd&7−4 ед. 512-круга ≈ ±2.8°) — был идеально прямой
                            double sa = ((int)(enemyRng() & 7) - 4) * (6.283185307 / 512.0);
                            double cs = std::cos(sa), sn = std::sin(sa);
                            spawnEnemyShot(a.x, a.y, a.floor, ux * cs - uy * sn, ux * sn + uy * cs, 1);
                            a.fireAnimT = 12; a.atkPose = 1; snd::playSfx(snd::enemyFireSfx(a.srcType)); }  // atkPose=1 → выстрел-вспышка a2
                    } else if (a.srcType == 0x6B) {                      // Boss2: ЗАЛП до 3 hitscan (тики 6/5/4), d0=0x1f4→9 (ZT 018db4)
                        if ((t == 6 || t == 5 || t == 4) && d < 4.0) { if (!bmiss) damagePlayer(9, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.atkPose = 1; snd::playSfx(snd::enemyFireSfx(a.srcType)); }  // atkPose=1 → удар a6
                    } else {                                             // ⭐Boss3 — ПЕРЕВЁРНУТО БЫЛО (аудит 2026-07-21, think 1a324):
                        // ОСНОВНАЯ атака = HITSCAN state2/3 ВСЕГДА (урон 0x1F4→9, стелс-пороги, дальняя);
                        // БЛИЖНИЙ удар (state1, ≤0x80=0.5кл, урон 0x100→12, звук 0x1b) — ТОЛЬКО при HP<0x7D0
                        // (=20 порт) И rnd бит7. Порт делал наоборот (ближний всегда, дальний только enrage).
                        if (t == 5) {
                            bool melee3 = (a.hp < 20) && (enemyRng() & 0x80) && d < 0.5;
                            if (melee3) { if (!bmiss) damagePlayer(12, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.atkPose = 0; snd::playSfxPolite(0x1b, 0x0F); }   // ближний a2
                            else if (d < 4.0) { if (!bmiss) damagePlayer(9, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.atkPose = 1; snd::playSfxPolite(0x1e, 0); }   // hitscan a4 (промах/выстрел 0x1E)
                        }
                    }
                }
                if (a.fireCd <= 0 && los) {
                    bool crouchMiss = enemyShotMiss(lvl.env(a.floor));  // стелс-гейт ZT: присед+тьма снижают шанс попадания
                    int fa0 = a.fireAnimT;                       // для детекции «только что выстрелил» (fireAnimT→12)
                    switch (ec) {
                        // EC_MELEE (Imp): удар ПЕРЕНЕСЁН в выпад-телеграф state2 выше (ZT 18992: один удар на суб-тике 5,
                        // НЕ контакт-по-кулдауну — Imp сперва ЗАМИРАЕТ в позе выпада на 10 тиков = юзер видит замах).
                        case EC_RANGED_FAST:                             // Revenant: контакт-рывок ≤1.6кл (ZT engage 0x12c)
                            if (d < 1.6 && !crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = ticks(30); } break;
                        // EC_RANGED (FH) / EC_GRENADIER (Sgt/FH-SF): атака ПЕРЕНЕСЕНА на суб-тики окна state2 выше
                        // (побайтный аудит 2026-07-21: ROM-цикл дистанс-зависимый, выстрел t5 + очередь t4/t3, БЕЗ fireCd).
                        // EC_BOSS_PROJ/EC_BOSS_MELEE: атака ПЕРЕНЕСЕНА в fire-окно (state2 суб-тики) выше — залп-серия, не одиночный fireCd.
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
                if (a.floor == cam.floor && gameDist(cam.px - a.x, cam.py - a.y) < 0.25) {    // достал игрока (октаг. ≤0x40=0.25кл, ZT 15fda; было 0.5 = вдвое толще)
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
                // ⭐ЭВИКЦИЯ ТРУПА (ROM 16094→14454: даль ≥10 кл → труп-celltype (+0x47) штампуется в грид, актёр free):
                // порт-эквивалент — труп переезжает в СТАТИК-мир (лежит на месте гибели, застывший кадр; пул свободен).
                // Вблизи труп остаётся актёром — дёргается/простреливается/пинается, как в ROM до эвикции.
                if (((s_evTick + (uint32_t)(&a - v.data())) & 7) == 0 &&
                    gameDist(a.x - cam.px, a.y - cam.py) >= evictDist) {
                    staticActors().push_back(a);                     // копия со всеми полями (drop/burned/variant/state)
                    a.active = false; break;
                }
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

    // ОБСЛУЖИВАНИЕ СТАТИК-МИРА: огни мерцают (frameT) и гаснут от пены (fireCd); трупы застывшие (кадр не тикает).
    for (Actor& sa : staticActors()) if (sa.active && sa.think == AT_FIRE) {
        ++sa.frameT;
        if (sa.fireCd > 0 && --sa.fireCd <= 0) sa.active = false;   // затухание после пены
    }

    // ОЧЕРЕДЬ СПРАЙТОВ: каждый актёр (пул + статик-мир) → билборд в worldFx (drawDecorBillboards, z-тест по стенам).
    auto& fx = worldFx(); fx.clear();
    // ⭐СТАТУИ СПЯЩИХ МАРКЕРОВ (ROM 9a6a: клетка спящего врага рендерится СТАТИЧНЫМ биллбордом врага
    // через 1131e — враги НЕ «появляются из ниоткуда»; оживают при dist<0x800=8 кл, эвикция обратно 10).
    // Стоячая поза 0 фронтом к камере; вариант/потолок — той же формулой, что даст будущий спавн.
    for (auto& m : pendingSpawns()) {
        if (m.floor != cam.floor) continue;
        uint8_t ct = m.ct ? m.ct : lvl.cellType(m.floor, m.x, m.y);
        if (!((ct >= 0x29 && ct <= 0x2B) || (ct >= 0x65 && ct <= 0x6B))) continue;
        int slot = enemyGfxSlot(ct);
        if (slot < 0 || slot >= 16 || !g_enemyAnim[slot].ok) continue;
        EnemyAnimSet& A = g_enemyAnim[slot];
        double mx = m.x + 0.5, my = m.y + 0.5;
        uint8_t variant = 0; double z = 0.0;
        if (ct == 0x65) { bool onCeil = ((m.x * 5 + m.y * 3) & 1); z = onCeil ? 1.0 : 0.0; variant = onCeil ? 1 : 0; }
        else if (ct == 0x2A) variant = (uint8_t)((m.x * 5 + m.y * 3) & 1);
        uint8_t dir = (uint8_t)enemyDirIndex(cam.px - mx, cam.py - my, cam.px - mx, cam.py - my, A.walkDirs);  // «идёт» на камеру = фронт
        fx.push_back({mx, my, cam.floor, 0, (int8_t)-8, 16, 7, slot, 0, false, (float)z, dir, 0, variant});
    }
    for (int pass = 0; pass < 2; ++pass)
    for (Actor& a : (pass ? staticActors() : v)) {       // non-const: walkFrame аккумулирует walkAcc (ROM $3c)
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
                    WorldFx wf{a.x, a.y, a.floor, A_BLOOD_FLY, (int8_t)(c - 2), 4, 2};        // h=4, w=2; центр по обеим осям (d2/d1 -= half)
                    wf.variant = a.variant; fx.push_back(wf); }
                else {
                    int age = a.timer; if (age > 255) age = 255;    // возраст $35 = 0..255
                    int w, h, top;
                    int c = (a.state == 2) ? 8 : (int)(8 - a.z * 16);   // пол: на полу (c=8); стена: на высоте z (сползает)
                    if (a.state == 2) {                             // ПОЛ (0x15a2c): W=(age+0x80)/64 растёт (лужа шире), H=(0x100−age)/256 площится
                        w = (128 + age) / 64;                       // frac16: age0=2 → age255≈6
                        h = (256 - age) / 256;                      // frac16: age0=1 → ~0 (ПЛОСКАЯ)
                        if (w < 1) w = 1; if (h < 1) h = 1;
                        top = c - h / 2;                            // ЦЕНТР по вертикали (ZT 15a2c: d2 -= h/2)
                    } else {                                        // СТЕНА (0x15a68): H=(age+0x100)/128 тянется вниз (потёк), W=(0x100−age)/256 сужается
                        h = (256 + age) / 128;                      // frac16: age0=2 → age255≈4
                        w = (256 - age) / 256;                      // frac16: age0=1 → ~0 (УЗКИЙ)
                        if (w < 1) w = 1; if (h < 1) h = 1;
                        top = c;                                    // ⭐ЯКОРЬ ВЕРХА у раны, тянется ВНИЗ (ZT 15a68: d2 НЕ трогается — только d1 -= w/2)
                    }
                    WorldFx wf{a.x, a.y, a.floor, A_BLOOD_SPLAT, (int8_t)top, (uint8_t)h, (uint8_t)w};
                    wf.variant = a.variant; fx.push_back(wf); }
                break; }
            case AT_FLAME: {                               // ⭐РАЗМЕР по дизасму (ROM draw 14150): height=s/2, width=s/4 (s=дист.scale),
                // на экране КВАДРАТ ~s/2 (дисплей растягивает X ×2). ФИКСИРОВАННЫЙ (по дистанции, НЕ по возрасту/lifetime!).
                // Было sz=3+timer/3, а timer=lifetime(255) → sz≈88 = ОГРОМНАЯ частица, перекрывала экран. Мерцание = hflip (ROM @14170 tick&1).
                int hF = 8, wF = 4;                        // frac16: hF·0.0625·Sd=Sd/2, wF·0.0625·cwd(2Sd)=Sd/2 → квадрат s/2 на экране
                int c = (int)(8 - a.z * 16);               // экранная высота по Z: арка из ствола (z≈0.45) вниз к полу (z=0)
                WorldFx wf{a.x, a.y, a.floor, a.tile, (int8_t)(c - hF/2), (uint8_t)hF, (uint8_t)wF};
                wf.wide = true; wf.variant = (uint8_t)(a.timer & 1); fx.push_back(wf); break; }  // wide=cwd; variant&1 → hflip-мерцание (ZT 14170)
            case AT_FIRE: {                                // ОГОНЬ на полу: МЕРЦАЕТ КАЖДЫЙ кадр. ⭐РАЗМЕР по дизасму (2026-07-15):
                int frame = a.frameT & 1;                 // тайл N / N+1 по (tick&1)
                uint8_t t = frame ? (uint8_t)(A_FIRE_TILE + 1) : A_FIRE_TILE;
                int hF, wF;
                if (a.timer < 0) { hF = 16; wF = 10; }    // КАРТА-хазард (вечный, ZT 115a0): height=s (ПОЛНАЯ клетка!), width=5s/8. Было sz~8 = крохотный
                else {                                    // ОГНЕМЁТ-огонь (ZT 14272): height=sc=s·(0x10−age)/16 → hF=timer(16→0), width=height/2
                    hF = a.timer < A_FIRE_LIFE ? a.timer : A_FIRE_LIFE; wF = hF / 2;
                }
                if (a.fireCd > 0) { hF = hF * a.fireCd / 10; wF = wF * a.fireCd / 10; }   // ЗАТУХАНИЕ от пены
                if (hF < 1) hF = 1; if (wF < 1) wF = 1;
                int8_t topOff = (int8_t)(8 - hF);         // НИЗ на полу (ZT d2 -= height)
                // wide=cwd (ширина ×2 дисплея): карта-огонь h=s/w=5s/8, огнемёт h=sc/w=sc/2 → на экране ШИРЕ (было квадрат Sd = вытянут вертикально).
                // overlay УБРАН: огонь клипится стеной (wallCloser) как всё → НЕ виден сквозь стены (юзер: в оригинале не просвечивает).
                WorldFx wf{a.x, a.y, a.floor, t, topOff, (uint8_t)hF, (uint8_t)wF}; wf.wide = true; fx.push_back(wf); break; }
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
                    // ⭐REVENANT СМЕРТЕЛЬНЫЙ СТАГГЕР (ZT draw 1adfe, HP<0): НЕ hit-аним, а ПАДЕНИЕ
                    // a8→a4→a6→a5 по фазе $35 (10 тиков). animSt=10, efr = стадия 0..3 (2 тика на стадию).
                    if (animSt == 2 && a.srcType == 0x66 && a.hp <= 0 && !A.revFall[0].empty()) animSt = 10;
                    // ⭐SGT МОРФ (ZT draw 1b628): кадр a6, суб = прогресс/2 кламп 4 (5 ступеней превращения)
                    if (a.srcType == 0x29 && a.state == 98 && !A.morph.empty()) {
                        int p = 9 - a.timer; if (p < 0) p = 0;                        // $35: 0..8 за 9 тиков морф-фазы
                        int sub = (p + 1) >> 1; int mn = (int)A.morph.size();
                        if (sub > 4) sub = 4; if (sub >= mn) sub = mn - 1;
                        fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, (uint8_t)sub, false, (float)a.z, 0, 6, a.variant});
                        break;
                    }
                    // ⭐BOSS3 ПРИТВОРСТВО (ZT draw 1a752/1a7dc): $35<5 → a10 лежит смирно; дальше RNG-дёрг a7 (15/16) / a8 (1/16)
                    if (a.srcType == 0x6A && a.state == 99 && !A.pretendLie.empty()) {
                        int p = 25 - a.timer; if (p < 0) p = 0;
                        uint8_t sub = 0; uint8_t st7;
                        if (p < 5) st7 = 7;                                            // лежит (pretendLie)
                        else { uint32_t r = (uint32_t)((a.frameT + p) * 2654435761u);
                               st7 = ((r >> 13) & 0xf) ? 8 : 9; }                      // дёрг A (15/16) / B (1/16)
                        fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, sub, false, (float)a.z, 0, st7, a.variant});
                        break;
                    }
                    if (a.srcType == 0x6A && a.state == 99) animSt = 3;               // фолбэк (кадры не декодировались) → death-поза
                    else if (a.srcType == 0x65 && a.state >= 3 && a.hitT <= 0 && !A.climb.empty()) animSt = 4;  // Hydaca ЛАЗАНИЕ (states 3-10) → DIRECTIONAL вертик. поза (ZT draw states 3-A)
                    else if (a.srcType == 0x65 && std::fabs(a.vz) > 0.001 && a.hitT <= 0 && !A.climb.empty()) animSt = 5;  // Hydaca ПАДЕНИЕ (баллистика vz≠0) → a1(вниз)/a8(вверх-срыв)/a10(мёртв) (ZT draw state2), НЕ directional
                    if (a.srcType == 0x68 && a.hitT <= 0) animSt = (a.state == 1) ? 1 : 0;  // Dog: боевая (бег a1/прыжок a2) ТОЛЬКО в погоне state1; отход state2 → ходьба a0 (ZT draw 198c4 state0/2→anim0)
                    // НАПРАВЛЕНИЕ: climb Hydaca = DIRECTIONAL по ракурсу к игроку (6 поз, ZT draw states 3-A по self↔cam); иначе по скорости
                    uint8_t dir;
                    bool hydClimb = (animSt == 4 && a.srcType == 0x65 && A.climbDirs > 0);
                    if (animSt == 5) dir = (uint8_t)((a.hp < 0) ? 2 : (a.vz > 0.0 ? 0 : 1));  // ПАДЕНИЕ: 0=вверх(a8) 1=вниз(a1) 2=мёртв(a10) (ZT 15402 по $2e/$36)
                    else if (hydClimb) dir = (uint8_t)hydClimbPose(a.state, a.x - cam.px, a.y - cam.py);  // ЛАЗАНИЕ: per-state геометрия (ZT 1539e 3-A), НЕ atan2
                    else {
                        // ⭐DIRECTIONAL и для огня/стаггера (ROM 1ba04 крутит виды у ЛЮБОГО кадра по velocity):
                        int dc = (animSt == 1 && !a.atkPose && A.fireDirs > 0) ? A.fireDirs
                               : (animSt == 2 && A.hitDirs  > 0) ? A.hitDirs : A.walkDirs;
                        dir = (uint8_t)enemyDirIndex(a.vx, a.vy, cam.px - a.x, cam.py - a.y, dc);
                    }
                    bool useFire2 = (a.atkPose && !A.fire2.empty());                              // 2-я боевая поза (fire2)
                    int nf = (animSt == 1) ? (int)(useFire2 ? A.fire2.size()
                                                 : (A.fireDirs > 0 && dir < (uint8_t)A.fireDirs && !A.fireD[dir].empty())
                                                     ? A.fireD[dir].size() : A.fire.size())
                           : (animSt == 2) ? (int)((A.hitDirs > 0 && dir < (uint8_t)A.hitDirs && !A.hitD[dir].empty())
                                                     ? A.hitD[dir].size() : A.hit.size())
                                           : (animSt == 4) ? (int)(hydClimb ? A.climbDir[dir].size() : A.climb.size())
                                           : (animSt == 5) ? (int)((dir==0) ? A.fallUp.size() : (dir==2) ? A.fallDead.size() : A.climb.size())
                                           : (int)A.walk[dir < A.walkDirs ? dir : 0].size();
                    if (nf < 1) nf = 1;
                    bool dogRun = (a.srcType == 0x68 && animSt == 1 && a.fireAnimT <= 0);           // Dog бег: цикл по движению, не по fire-таймеру
                    uint8_t efr;
                    if (animSt == 5) {
                        // ⭐ПАДЕНИЕ (ZT draw 15402): ФИКС-суб-кадры, НЕ цикл по таймеру («дёргается иначе» — юзер):
                        // a8 вверх → d2=1 (суб1); a1 вниз живая → d2=0 (суб0); МЁРТВАЯ → кадр-труп:
                        // суб0 пока ВЫСОКО (Z>0x10=0.0625кл), суб1 у пола — двухфазный «шлёп», без прокрутки.
                        efr = (dir == 0) ? (uint8_t)(nf > 1 ? 1 : 0)
                            : (dir == 1) ? (uint8_t)0
                            : (uint8_t)((a.z > 0.0625 || nf < 2) ? 0 : 1);
                    } else if (dogRun) efr = walkFrame(a, nf);                                    // бег Dog: дистанс-модель
                    else if (a.srcType == 0x2B && animSt == 1) efr = walkFrame(a, nf);            // Imp замах/удар: суб-кадры = дистанс-ходьба (ROM 18aa0 d2=−1)
                    else if (animSt == 10) {                                                      // Revenant-падение: стадия по 2 тика (ROM $35 10→0)
                        int st = a.hitT / (ticks(2) > 0 ? ticks(2) : 2); if (st > 3) st = 3;
                        efr = (uint8_t)st;
                    }
                    else if (animSt == 1) efr = (uint8_t)(((12 - a.fireAnimT) / 3) % nf);          // стрельба: прогон кадров
                    else if (animSt == 2) efr = (uint8_t)((a.hitT / 3) % nf);                      // стаггер (hitT растёт)
                    else efr = walkFrame(a, nf);
                    // ⭐ХОДЬБА = ДИСТАНС-АККУМУЛЯТОР (ZT 1ba04 @1bb6a): суб-кадр по ПРОЙДЕННОМУ ПУТИ через паттерн-LUT
                    // 1bbaa, стоящий враг — всегда стоячая поза 0 (было (frameT/6)%nf — время, шаги «скользили»).
                    WorldFx wfe{a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, efr, false, (float)a.z, dir, animSt, a.variant};
                    wfe.atkPose = a.atkPose; fx.push_back(wfe);
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
                    if (a.burned && !g_burntRemains.empty()) {        // ⭐СПАЛЁННЫЙ → BURNT REMAINS (ZT draw 1b952): 2-кадр мерцание (tick/4&1), +3 простреленный
                        int nb = (int)g_burntRemains.size();
                        efr = (uint8_t)((((a.state & 1) ? 3 : 0) + ((a.frameT >> 2) & 1)) % nb);
                    } else if (slot == 1 && nd > 1) {                 // FH: LUT-мерцание + фаза простреливания
                        static const uint8_t FHLUT[16] = {0,0,0,0,2,2,2,2,4,4,4,4,2,2,2,2};
                        uint8_t f = FHLUT[(a.frameT >> 1) & 0xf]; if (f >= nd) f = (uint8_t)(nd - 1);
                        efr = (uint8_t)((f + (a.state & 1)) % nd);
                    } else if (slot == 3 && nd > 1) {                 // ⭐Hydaca corpse-draw 0x15618 (capstone, ПЕРЕСМОТР по юзеру 2026-07-21):
                        // ДВА состояния, ОБА анимированы RNG-битом каждый тик: свежий труп = кадры 0/1;
                        // ПРОСТРЕЛЕННЫЙ = НЕОБРАТИМО (ROM: receive 1570c ставит state2, а трение 156d2 state
                        // НЕ сбрасывает → +2 перманентен; «обратно не собирается») = кадры 2/3, при нечётном
                        // числе прострелов ещё +4 (parity $35) → 6/7 (визуально та же пара «раскрытый»).
                        // ⭐ЧАСТОТА (юзер-поправка №3, дизасм 15618: andi #$f00; beq): база = суб-кадр 1 в 15/16
                        // тиков, ДЁРГ (суб-кадр 0) лишь в 1/16 (~раз в секунду при 15Гц) — НЕ 50/50 мельтешение.
                        uint32_t r = (uint32_t)(a.frameT * 1103515245u + 12345u);
                        int rb = (((r >> 16) & 0xF) != 0) ? 1 : 0;    // 15/16 → 1 (лежит), 1/16 → 0 (дёрг)
                        efr = (uint8_t)(rb + (a.state >= 1 ? 2 : 0) + ((a.state & 1) ? 4 : 0));
                        if (efr >= nd) efr = (uint8_t)(2 + rb);       // пара 6/7 не декодировалась → «раскрытая» пара 2/3
                        if (efr >= nd) efr = (uint8_t)(nd - 1);
                    } else if ((slot == 2 || slot == 7) && nd > 1) {  // Imp/FH-SF: кадр 0 (при простреле → 1)
                        efr = (uint8_t)((a.state & 1) && nd > 1 ? 1 : 0);
                    } else if (slot == 4) {                           // Revenant: hHit умирающего (ZT 1b030) кадр НЕ меняет —
                        efr = 0;                                      //   всегда суб0 кадра 7 (прострел = только нокбэк-скольжение)
                    } else {                                          // статичный труп: кадр 0 (не последний!)
                        efr = (uint8_t)((a.state & 1) && nd > 1 ? 1 : 0);   // 1-кадровые → всегда 0; многокадр. Sgt → 0/1
                    }
                    fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, efr, !hasDeath, (float)a.z, 0,
                                  (uint8_t)(hasDeath ? 3 : 0), a.variant, false, a.burned});   // burned → обугленный труп
                }
                // ⚠ БЕЗ видимого напольного пикапа: труп САМ носит оружие (ROM corpse-think 186fa). Отдельный
                // объект-пикап на полу (cmd 0x13→1c03a) спавнится ТОЛЬКО в 2P-link (ветка a3≠игрок @18774), не
                // в одиночной игре. Оружие берётся НАСТУПАНИЕМ на труп (corpsePickup, дистанция <0x60≈0.375 кл).
                break; }
            default: break;
        }
    }
}
