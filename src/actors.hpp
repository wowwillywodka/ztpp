// ztpp — СИСТЕМА АКТЁРОВ + ОЧЕРЕДЬ РЕНДЕРА (порт actors.asm + render queue 1c03a/1c52c).
//
// ZT: пул 64 слота ($FF11E8) + связный список; поля актёра +0x12 think-ptr, +0x1f таймер, +0x20/22 поз,
// +0x2a/2c скорость, +0x32 тип, +0x34 state, +0x3e тайл-кадр. Апдейт списка FUN_131e8 (декремент таймера →
// think), спавн 13466, free 13526, инит 133a4. Снаряды/эффекты и враги — ВСЁ актёры одного пула.
//
// ОЧЕРЕДЬ РЕНДЕРА (1c03a enqueue → 1c52c flush+dispatch, 27 draw-классов): актёр не рисует себя сам, а кладёт
// СПРАЙТ-ДЕСКРИПТОР (тип, X/Y, кадр, палитра). В софт-порте кольцевой буфер не нужен — суть (собрать спрайты →
// сортировка по глубине → блит с z-тестом) уже воплощена в worldFx + drawDecorBillboards (raycaster.hpp).
// updateActors() = «think всех» + наполнение очереди спрайтов (worldFx) для рендера.
#pragma once
#include "level.hpp"
#include "cells.hpp"
#include "gamedata.hpp"
#include "raycaster.hpp"          // worldFx, Camera, cellBlocks
#include "sound.hpp"              // snd::ev — звук взрыва
#include <vector>
#include <unordered_set>
#include <cmath>

// Тип поведения (+0x12 think в ZT). Эффекты + враги в одном пуле.
enum ActorThink {
    AT_FREE = 0,
    AT_BULLET,        // летящий снаряд (ракета/граната/мина) → взрыв о стену
    AT_EXPLOSION,     // взрыв ракеты/гранаты (на месте, гаснет)
    AT_SPARK,         // искра стрелкового импакта (мелко, коротко)
    AT_FLAME,         // частица пламени огнемёта (летит, гаснет)
    AT_ENEMY,         // враг (билборд + HP + простой AI)
    AT_DEATH,         // эффект смерти врага
    AT_FIRE,          // ВРЕМЕННЫЙ ОГОНЬ огнемёта (горит на месте, тухнет, жжёт врагов) — think 0x142c6
    AT_MINE,          // поставленная мина (лежит на полу)
    AT_GRENADE,       // граната: дуга+гравитация+отскок от стен/пола, фитиль → взрыв (think 0x13adc)
    AT_ENEMY_SHOT,    // снаряд ВРАГА (летит в игрока → урон по близости, как ZT проектиль 0x13adc→16294)
    AT_CORPSE,        // труп врага (лежит на полу после смерти; ZT штампует celltype в карту 0x14a56)
    AT_FOAM,          // ПЕНА огнетушителя (ZT 0x12cfe think 0x1423a): летит вперёд, тушит огонь, БЕЗ урона
    AT_BLOOD,         // КРОВЬ: частица брызг попадания (ZT spawn 0x157ca → think 0x158cc): летит+гравитация+
                      //   разброс, тухнет в брызг-пятно о стену(state1)/пол(state2). Тайл 61 лёт / 62 пятно.
};

struct Actor {
    bool    active = false;
    double  x = 0, y = 0;      // позиция (клетки)   +0x20/+0x22
    double  vx = 0, vy = 0;    // скорость (кл/кадр)  +0x2a/+0x2c
    double  z = 0, vz = 0;     // высота над полом + верт.скорость (граната: $24/$2e гравитация)
    int     floor = 0;
    int     think = AT_FREE;   // тип поведения       +0x12
    int     state = 0;         //                     +0x34
    int     timer = 0;         // возраст/таймер/фитиль +0x1f / $34
    int     hp = 0;            // здоровье (враги)
    int     hitT = 0;          // СТАГГЕР/отлёт от удара (кадры): скользит по vx/vy, не атакует (ZT state=2)
    int     fireCd = 0;        // кулдаун стрельбы врага (timer занят под фазу-таймер ИИ)
    int     fireAnimT = 0;     // таймер анимации стрельбы/удара врага (показывает fire-аним ~кадров)
    int     xformT = 0;        // таймер трансформации (Sgt 0x29 → FH-SF 0x69 по истечении, ZT state6)
    uint8_t variant = 0;       // визуальная вариация (FH: 0/1 → walk vs walkB; «две вариации при одном cell id»)
    bool    revived = false;    // Boss3 (0x6A): воскрешение ОДИН раз при HP≤0 (ZT state6 0x1a40c → HP=0x3e8)
    uint8_t patrolDir = 0;      // Revenant: текущее направление сеточного патруля (0..7, ZT нав 0x1ac76)
    double  aimX = 0, aimY = 0;// ТОЧКА ПРИЦЕЛА врага (ZT +0x38/+0x3a): игрок+скорость+rnd ±1 клетка; движется К НЕЙ, не к игроку
    double  homeX = 0, homeY = 0; // точка СПАВНА (для патруля собак у дома, не дрейфа к игроку)
    int     frameT = 0;        // таймер анимации ходьбы (растёт при движении)
    int     walkAcc = 0;       // ⭐ROM $3c: ДИСТАНС-аккумулятор анимации ходьбы (+= октаг.скорость/тик, ед. 256/кл)
    uint8_t atkPose = 0;       // 0=основная боевая аним (fire), 1=2-я поза (fire2: прыжок/выстрел/дальний/удар/бросок) — ставится в момент атаки
    uint8_t tile = 0;          // тайл объект-банка   +0x3e
    uint8_t srcType = 0;       // исходный celltype врага
    int     drop = -1;         // ТРУП: оброненное оружие (id 1..14), подбирается при шаге на труп; −1 = нет
    bool    burned = false;    // обгорел в огне → труп «сожжённый» (тёмный/обугленный)
    int      gunBurst = 0;     // ОЧЕРЕДЬ выстрела hitscan-человека: 0x68+0x97 ×3, интервал ~120мс (MAME: 0.1-0.15с). По ВРЕМЕНИ — think идёт на переменном FPS (по умолч. 15!)
    uint32_t gunBurstNext = 0; // SDL_GetTicks() времени следующего залпа
};

// ── ПУЛ (ZT: 64 слота). ФИКСИРОВАННЫЙ размер (без push_back!) — спавн во время think не должен
// реаллоцировать вектор и инвалидировать итераторы (взрыв гранаты спавнит актёров прямо в цикле). ──
static const int MAX_ACTORS = 256;
inline std::vector<Actor>& actors() { static std::vector<Actor> v(MAX_ACTORS); return v; }
// ⭐СТАТИК-МИР вне пула (ROM: дальние трупы и карта-огни = ГРИД-КЛЕТКИ, не актёры): эвиктированные
// трупы (ROM 14454: труп-celltype +0x47 = 0x2C/0x6C-0x74 штампуется в пустую клетку 3×3, актёр free;
// клетка рендерится статичным биллбордом 944E и ПЕРСИСТЕНТНА — трупы при уходе с этажа НЕ стираются)
// и вечные огни 0x18 (в ROM — грид-декор). Обновляется минимально (мерцание), рисуется вместе с пулом.
inline std::vector<Actor>& staticActors() { static std::vector<Actor> v; return v; }
inline void clearActors() { for (auto& a : actors()) a.active = false; staticActors().clear(); worldFx().clear(); }
// alloc по ROM 13466: пул полон → NULL, спавн ПРОВАЛИВАЕТСЯ. НИКОГДА не затираем занятый слот
// (старый фолбэк «перезаписать слот 0» стирал живых врагов при переполнении — «пропавшие Hydaca»).
inline Actor* allocActorPtr() {
    for (auto& a : actors()) if (!a.active) { a = Actor{}; a.active = true; return &a; }
    return nullptr;
}
inline Actor& allocActor() {                 // обёртка для эффектов: провал → scratch вне пула (эффект молча не появится — как в ROM)
    if (Actor* p = allocActorPtr()) return *p;
    static Actor scratch; scratch = Actor{}; return scratch;   // active=false → не в пуле, не обновляется, не рисуется
}
// Число живых врагов на этаже (для сообщений «FLOOR SECURED» / «ZERO ENEMIES REMAINING»).
inline int aliveEnemies(int floor) {
    int n = 0;
    for (auto& a : actors()) if (a.active && a.think == AT_ENEMY && a.floor == floor) ++n;
    return n;
}

// ── HP ИГРОКА (ZT $FF0DE4 = -0x721c, старт/макс 0x64=100; i-frame $FF0DE2 = -0x721e; урон d800) ──
struct PlayerState {
    int    hp = 100, maxHp = 100;    // ZT: старт/макс = 0x64 (100)
    // ⭐ВСПЫШКА УРОНА/СМЕРТИ = ROM $FF1072 (VBlank 0xB12): CRAM-СЛОВО, пишется в слот 63 (линия 3 цвет 15) =
    // ЧЁРНЫЙ ФОН КОКПИТА. Сцена (линии 0/1) НЕ трогается! Урон (d87c): max(тек, 15−(X>>7)) ∈ 8..15 = красный.
    // Смерть (1756): 0xFFF = белый → −0x110/кадр (гаснут B+G, 15 кадров до 0x00F=красный) → −1/кадр до чёрного.
    int    flashCram = 0;            // 0 = нет вспышки; иначе сырое CRAM-слово для слота 63
    int    armor = 0;               // БРОНЯ: пул 0-100% (жилет в инвентаре = ammo[3]·10). Поглощает урон ПОЛНОСТЬЮ
                                    //   пока >0 (HP НЕ падает), −10% за попадание; кончилась → урон в HP. (юзер: «по процентам, −10%/хит»)
    bool   fireImmune = false;       // огнезащ. костюм — иммунитет к огню (ставит main)
    bool   godmode = false;          // чит
    bool   dead = false;             // hp дошёл до 0 в этом кадре (main обрабатывает респаун)
    double knockVx = 0, knockVy = 0; // ОТСКОК от источника урона (d800: вектор от источника, затухает)
    int    knockTimer = 0;           // НОКДАУН (сильный удар): кадры «лежания» (ZT -0x71dc=0x14=20)
    double knockPitch = 0;           // питч взгляда при нокдауне → в пол (ZT -0x71e4=-0x10), плавно ±/кадр
    // ПРЫЖОК (вверх+A) / ПРИСЕД (вниз+A): питч = ВЕРТИКАЛЬНОЕ положение камеры (ZT -0x71e6, НЕ наклон).
    double jumpVel = 0, jumpY = 0;   // прыжок: импульс +9, гравитация −2/кадр (ZT -0x71e0→-0x71e6), парабола вверх
    double crouchY = 0;              // присед: питч едет вниз (−16) пока держишь, как ZT (-0x71e4)
    int    lastCellKey = -1;         // клетка игрока в прошлом кадре (ZT -0x71ea): step-on-хазарды (огонь) срабатывают при СМЕНЕ клетки
};
inline PlayerState& player() { static PlayerState p; return p; }
inline void resetPlayerHP() { PlayerState& p = player(); p.hp = p.maxHp; p.flashCram = 0; p.dead = false;
    p.knockVx = p.knockVy = 0; p.knockTimer = 0; p.knockPitch = 0; p.jumpVel = p.jumpY = 0; p.crouchY = 0; p.lastCellKey = -1; }
inline int& playerFighter();   // fwd (тело ниже) — боец U-RON для перка урона в damagePlayer
// Урон игроку (как d800): броня + вспышка палитры + ОТСКОК от источника (sx,sy) + смерть.
// ⭐БЕЗ i-frames (VERIFIED 2026-07-22): ROM d800 гейтится ТОЛЬКО -$721e = СЧЁТЧИК СМЕРТИ (174a: ++ до 0x1E
// при гибели; «мёртвый не получает урон») — НЕ инвулн! Стан -$71e0 (d900[idx][0], сильные удары) блокирует
// лишь ПОВТОРНЫЙ нокбэк/питч-кик (d88e), урон проходит КАЖДЫЙ раз. Темп урона задают пер-вражьи кулдауны
// (Imp окно/Hydaca $41=10/Dog отступ), а контакт Revenant (2 HP/тик) бьёт каждый тик — потому в оригинале
// набегание на мили-врагов убивает быстро (фидбэк юзера). Порт-iframe=20 был отсебятиной.
// godmode = ЗАМОРОЗКА HP: удар ОБРАБАТЫВАЕТСЯ (вспышка/отскок), но HP не уменьшается.
inline void damagePlayer(int dmg, double px, double py, double sx, double sy) {
    PlayerState& p = player();
    if (p.hp <= 0 || dmg <= 0) return;               // ROM d800: tst -$721e (смерть), не инвулн
    // ⭐СТОЙКА модифицирует СЫРОЕ X ДО таблицы урона/нокбэка (ZT d800 @d80e, до d834). dmg = 16−(X>>6) → X ≈ (16−dmg)<<6.
    //   ПРЫЖОК (jumpY>0, d816 X×0.75) → X МЕНЬШЕ → урон И НОКБЭК БОЛЬШЕ (в воздухе рискованнее). ПРИСЕД (crouchY<0, d820 X×1.5)
    //   → X БОЛЬШЕ → урон+нокбэк МЕНЬШЕ, при X≥0x400 — УВОРОТ без урона (d82a RTS). ⚠ Прежняя правка «стойка на УРОН ×0.75/×1.5»
    //   имела ОБРАТНОЕ направление (память неверно прочла d816: `d1-=d1/4` = X×0.75, а не «урон ×0.75»). X реконструирован из
    //   dmg (низкие 6 бит теряются → порог уворота приближён; для пиксель-точного присед-уворота нужно передавать сырое X).
    int X = (16 - dmg) << 6;
    double stance = p.jumpY + p.crouchY;
    if      (stance > 0.0) X -= X / 4;       // ПРЫЖОК: X×0.75 (d816)
    else if (stance < 0.0) X += X / 2;       // ПРИСЕД: X×1.5 (d820)
    if (X >= 0x400) return;                  // d82a: X≥0x400 → УВОРОТ (присед уводит от слабых атак; без урона/нокбэка)
    int idx = X >> 6; if (idx < 0) idx = 0; if (idx > 15) idx = 15;   // индекс таблицы = X_стойка >> 6
    int hpDmg = 16 - idx;                    // урон после стойки (d840: 16 − X_стойка>>6)
    int fg = playerFighter();
    if      (fg == 3) hpDmg -= hpDmg / 4;    // боец3 перк: урон ×0.75 (d84e)
    else if (fg == 4) hpDmg >>= 1;           // боец4: урон ×0.5 (d85c)
    if (hpDmg < 0) hpDmg = 0;
    // БРОНЯ поглощает удар ЦЕЛИКОМ (HP не падает), −10% за попадание; кончилась → урон идёт в HP (юзер verif.).
    bool absorbed = false;
    if (!p.godmode && p.armor > 0) { p.armor -= 10; if (p.armor < 0) p.armor = 0; absorbed = true; }
    if (!p.godmode && !absorbed) { p.hp -= hpDmg; if (p.hp < 0) p.hp = 0; }   // god: HP заморожен; броня: HP не трогаем
    // ⭐БАГ-ФИКС: с жилетом ветка красной вспышки -$6f8e ПРОПУСКАЕТСЯ (ZT d876 bpl d88e) — фон НЕ краснеет, пока броня держит.
    // ⭐ВСПЫШКА (ROM d87c): $FF1072 = max(текущее, 15 − X_стойка>>7) ∈ 8..15 — КРАСНОЕ CRAM-слово (R=4..7) для
    // слота 63 (чёрный фон кокпита); сильный удар (idx мал) → ярче. Затухание −1/VBlank в main (как 0xB12).
    if (!absorbed) { int fv = 15 - idx / 2; if (fv > p.flashCram) p.flashCram = fv; }
    // ⭐НОКБЭК (d800 @D89E, ТАБЛИЦА @0xD900): сила = |force[idx]|/256 кл/тик, idx = X_стойка>>6 → ВАРЬИРУЕТСЯ ПО СТОЙКЕ (юзер:
    //   «в игре вариабельно»): Hydaca стоя idx10=81 / прыжок idx8=100 (сильнее) / присед — слабее/уворот. Затухание ×¾/кадр (dd46). Вектор ОТ источника.
    static const int KNOCK_FORCE[16] = {136,135,133,130,126,121,115,108,100,91,81,70,58,45,31,16};  // |force| из D900[idx]+$2
    double dx = px - sx, dy = py - sy, d = std::hypot(dx, dy);   // отскок ОТ источника
    if (d > 0.01) { double k = KNOCK_FORCE[idx] / 256.0; p.knockVx = dx / d * k; p.knockVy = dy / d * k; }
    if (idx <= 2) p.knockTimer = 20;         // НОКДАУН (питч в пол) при stun≥7 (D900 idx≤2 = сильнейшие: взрывы; ZT d8d2)
    if (p.hp == 0) p.dead = true;
}
inline void healPlayer(int amt) { PlayerState& p = player(); p.hp += amt; if (p.hp > p.maxHp) p.hp = p.maxHp; }

// ── ЭФФЕКТ-параметры ──
static const int A_EXPL_FRAMES  = 8;
static const int A_SPARK_FRAMES = 6;     // искра стрелкового: 3 кадра анимации 63→64→65 (по 2 тика)
static const int A_FIRE_LIFE    = 16;    // наземный огонь живёт 16 тиков ($1e 0→0x10, дизасм think 0x1423a) + сжимается с возрастом
static const uint8_t A_EXPL_TILE  = 1;   // огненный шар (взрыв ракеты/гранаты)
static const uint8_t A_FLAME_TILE = 1;   // частица огнемёта (ZT draw 0x14150 → объект-тайл 1, как ракета/шар)
static const uint8_t A_SPARK_SEQ[3] = {63, 64, 65};  // искра пули: вспышка→кольцо→рассеивание (объект-банк)
static const uint8_t A_FIRE_TILE  = 0;   // пламя/огонь (объект-банк 0)

// ── СПАВН эффектов ──
inline void spawnBullet(double x, double y, int f, double dx, double dy, double sp, uint8_t tile, int life) {
    Actor& a = allocActor(); a.think = AT_BULLET; a.x = x; a.y = y; a.vx = dx * sp * simDt(); a.vy = dy * sp * simDt();
    a.floor = f; a.tile = tile; a.timer = life;
}
inline uint32_t enemyRng();  // fwd-декл (тело ниже) — для разброса частиц огнемёта
// ⭐ОГНЕМЁТ (ZT draw 0x1282e + think 0x142c6): КАЖДЫЙ кадр спавнит частицу-пламя. Velocity = view-dir/4 (0.25 кл/кадр)
// + СЛУЧАЙНЫЙ разброс ±0x10 units (±0.0625 кл) по каждой оси (RNG a5c) = «дрожащая струя». z на уровне глаз,
// z-vel −2 units → всплывает, гравитация +1/кадр опускает. Тайл = 1 (объект-банк, ZT draw 14150, как ракета).
// Частица САМА жжёт врагов в ≤1 клетке (не отдельный трейл). Спрайты стены НЕ поджигает.
inline void spawnFlameP(double x, double y, int f, double dx, double dy, double targetDist = -1.0) {
    Actor& a = allocActor(); a.think = AT_FLAME; a.x = x; a.y = y; a.floor = f;
    double sx = ((int)(enemyRng() & 0x1f) - 0x10) / 256.0;  // ZT 129d4: ±0x10 units разброса (jsr a5c) по КАЖДОЙ оси
    double sy = ((int)(enemyRng() & 0x1f) - 0x10) / 256.0;
    a.vx = (dx * 0.25 + sx) * simDt(); a.vy = (dy * 0.25 + sy) * simDt();           // aimVec/4 + разброс (ZT 129b6: -$7200>>2)
    a.z = 0.45; a.vz = 0.03;                                // из ствола (чуть ниже глаз); zVel вверх (ROM 0x1299a: zVel=−2) → всплыв→опад
    a.tile = A_FLAME_TILE; a.frameT = 0;
    // ⭐LIFETIME (ZT 12994): прицельный конус нашёл врага → (dist>>7)+1 тиков (частица импактится У него) → AoE-урон достаёт;
    //   без цели → 0xFF (fallback: реально падает по арке за ~10 тиков). Раньше фикс 15 БЕЗ наведения → урон мимо.
    a.timer = (targetDist > 0) ? (((int)(targetDist * 256.0) >> 7) + 1) : 255;
}
// ОГОНЬ-ОБЪЕКТ (ZT think 0x142c6): горит на месте, жжёт горючих в ~1кл. fuse<0 = ВЕЧНЫЙ (клетка-хазард 0x18),
// иначе lifetime (огонь от огнемёта). playerSafe=true (огонь ИГРОКА из огнемёта) → НЕ жжёт игрока (нет само-урона);
// карта-хазард 0x18 (playerSafe=false) → жжёт игрока. Тушится пеной. dedup по позиции. state: 1=safe / 0=hazard.
inline void spawnFire(double x, double y, int f, int fuse, bool playerSafe = false) {
    for (auto& e : actors()) if (e.active && e.think == AT_FIRE && e.floor == f &&
                                 std::fabs(e.x - x) < 0.5 && std::fabs(e.y - y) < 0.5) { if (fuse > e.timer) e.timer = fuse; return; }  // уже горит тут
    Actor& a = allocActor(); a.think = AT_FIRE; a.x = x; a.y = y; a.floor = f; a.tile = A_FIRE_TILE;
    a.timer = fuse; a.frameT = (int)((x * 7 + y * 13)) & 7;   // разнобой мерцания
    a.state = playerSafe ? 1 : 0;                             // 1 = огонь игрока (без само-урона)
}
// ПЕНА огнетушителя (ZT 0x12cfe→think 0x1423a/draw 0x14272): БЕЗ разброса — одиночный спрайт пены (декор-тайл 0),
// появляется перед стволом и ПАДАЕТ ВНИЗ (draw +0x20 вниз), сжимается за ~16 кадров (ZT $1e 0..0x10), БЕЗ урона; гасит огонь.
inline void spawnFoam(double x, double y, int f, double dx, double dy) {
    Actor& a = allocActor(); a.think = AT_FOAM; a.x = x; a.y = y; a.floor = f;
    a.vx = dx * 0.12 * simDt(); a.vy = dy * 0.12 * simDt();       // лёгкий нос вперёд
    a.z = 0.45; a.vz = -0.05;                 // из дула на уровне рук → ПАДАЕТ вниз (гравитация в think)
    a.timer = 16;                             // ZT: $1e 0..0x10, спрайт сжимается с возрастом
}
inline void spawnMine(double x, double y, int f) {
    Actor& a = allocActor(); a.think = AT_MINE; a.x = x; a.y = y; a.floor = f; a.tile = 8;  // мина (объект-банк 8)
    a.timer = 21;   // АРМИНГ (ZT 0x12bfe: ставит actor+$1f=0x14 → первый think через ~21 кадр) — НЕ детонирует сразу на ставившем
}
// Граната (think 0x13adc): бросается с дугой (z вверх + гравитация), горизонт. скорость, фитиль $34=50.
// enemyBallistic (ZT 1b40a/19dba): dx/dy = ПОЛНЫЙ вектор к игроку (кл); vel = вектор/16 (дальше игрок → сильнее навес =
//   НАСТИЛЬНОСТЬ по дистанции), спавн-поз += вектор/32. Игрок (owner=0): единичный dx/dy × фикс. 0.12·rangeMul.
inline void spawnGrenade(double x, double y, int f, double dx, double dy, int owner = 0, double rangeMul = 1.0, bool enemyBallistic = false) {
    Actor& a = allocActor(); a.think = AT_GRENADE; a.floor = f;
    if (enemyBallistic) {                                          // ВРАГ: дист-зависимый навес
        a.x = x + dx / 32.0; a.y = y + dy / 32.0;                 // спавн из «руки» вперёд (ZT +вектор/32)
        a.vx = dx / 16.0; a.vy = dy / 16.0;                       // vel=вектор/16 — ложится у игрока за ~16 тиков
    } else {
        a.x = x; a.y = y;
        a.vx = dx * 0.12 * rangeMul * simDt(); a.vy = dy * 0.12 * rangeMul * simDt(); // горизонт. бросок (боец1: дальность ×2, ZT 12e7c)
    }
    a.z = 0.35; a.vz = 0.028;               // из руки, невысокая дуга
    a.timer = 50; a.tile = 16;              // фитиль 50 тиков ($34); ЛЕТЯЩАЯ граната = декор-тайл 16 (ZT draw 0x13f3e bank 0x1109be)
    a.state = owner;                        // 0=граната игрока (детонир. у врага), 1=граната врага (детонир. у игрока)
}
// ⭐Граната С АВТОНАВЕДЕНИЕМ (ZT 11e0a-11e3c): vel = вектор к упреждённой цели / фитиль; фитиль = dist/64+8 ROM-тиков
// (баллистика «ложится на цель» расчётом фитиля, proximity-взрывателя у гранаты НЕТ).
inline void spawnGrenadeAimed(double x, double y, int f, double vx, double vy, int fuse) {
    Actor& a = allocActor(); a.think = AT_GRENADE; a.floor = f; a.x = x; a.y = y;
    a.vx = vx; a.vy = vy; a.z = 0.35; a.vz = 0.028;
    a.timer = fuse; a.tile = 16; a.state = 0;
}
inline void spawnSparkA(double x, double y, int f) {
    Actor& a = allocActor(); a.think = AT_SPARK; a.x = x; a.y = y; a.floor = f; a.tile = A_SPARK_SEQ[0];
}
inline Actor& spawnExplosionA(double x, double y, int f, double z = 0.5) {  // z = визуальная высота (0 пол, 0.5 ур.глаз)
    Actor& a = allocActor(); a.think = AT_EXPLOSION; a.x = x; a.y = y; a.floor = f; a.tile = A_EXPL_TILE; a.z = z;
    return a;
}
inline void spawnDeath(double x, double y, int f) {
    Actor& a = allocActor(); a.think = AT_DEATH; a.x = x; a.y = y; a.floor = f; a.tile = A_EXPL_TILE;
}
// ── КРОВЬ (ZT spawn 0x157ca → think 0x158cc) ──────────────────────────────────────────────────────
// При попадании во врага ZT разбрасывает частицы крови (d0=0x400 → 4 шт/вызов; на убойный хит — ×2 = 8).
// Каждая: позиция врага, скорость = направление удара + RND-разброс (±0x40 в 8.8 = ±0.25 кл/кадр),
// z-смещение $24 (RND −8..+7), z-скорость $2e (RND −9..+6), гравитация −2/кадр, гориз.-трение ×0.75/кадр.
// Граф.: тайл 61 (0x1163be) в лёте, тайл 62 (0x1165be) пятно о стену/пол. ZT держит ≤16 частиц (-$700a==0x10).
static const uint8_t A_BLOOD_FLY  = 61;   // 0x10E9BE + 61·0x200 = 0x1163BE — летящая капля (спрей)
static const uint8_t A_BLOOD_SPLAT = 62;  // 0x10E9BE + 62·0x200 = 0x1165BE — пятно-клякса (стена/пол)
inline double bloodRnd() { static uint32_t s = 0x2468ace1u; s = s * 1664525u + 1013904223u; return ((int)((s >> 16) & 0xFF) - 128) / 128.0; }   // −1..~+1
// Число частиц крови по ROM-урону d0 (ZT 0x157ca: d7=((d0−0x338)>>6)+1; отсечка d7<0 или d7>4 → 0 частиц;
// затем dbra d7 крутит цикл d7+1 раз). Итог 1..5 частиц за вызов на урон ≥~0x2F8.
inline int bloodParticleCount(int romDmg) {
    int d7 = ((romDmg - 0x338) >> 6) + 1;                     // asr.w #6 — арифметический сдвиг (для отрицательных)
    if (d7 < 0 || d7 > 4) return 0;                            // bmi / bhi #4 → крови нет
    return d7 + 1;                                             // dbra d7 → d7+1 итераций
}
// ⭐КЛАССИФИКАЦИЯ КРОВИ [VERIFIED 2026-07-14 по objdef-таблице ROM @0xAB0C (receive = think+8) + ИСЧЕРПЫВАЮЩЕМУ скану
// 11 вызовов 0x157ca в 0x14000-0x1c000, вкл. СПРЯТАННЫЕ в code-as-data]. ⚠Прежняя запись была НЕВЕРНА (текст-grep видел
// лишь 6 из 11 — 5 в data-масках; ошибочно: Revenant кровил, Imp/Dog нет). Кровят РОВНО «органика», Hydaca/Revenant (роботы) — НЕТ:
//   hit-gib (одиночный, count по урону, receive зовёт 0x157ca 1× при убойном ≥0x338): Sgt 0x29 (0x1b824), FH 0x2A (0x187c8),
//   Imp 0x2B (0x18b12), Dog 0x68 (0x1999a), FH-SF 0x69 (0x1a154).
inline bool enemyBleeds(uint8_t ct) {
    return ct==0x29 || ct==0x2A || ct==0x2B || ct==0x68 || ct==0x69;   // Sgt, FH, Imp, Dog, FH-SF
}
// БОССЫ: receive на убойном ударе (HP из ≥0 в <0) — ДВОЙНОЙ 0x157ca с d0=0x400 → фонтан (~8-10 частиц), БЕЗ гейта ≥0x338:
//   Boss1 0x67 (0x1947e→0x194da/e2), Boss2 0x6B (0x18f9e→0x18ffa/02), Boss3 0x6A (0x1a84a→0x1a8a6/ae). НЕ hit-gib (тут death-only).
inline bool enemyDeathGush(uint8_t ct) {
    return ct==0x67 || ct==0x6A || ct==0x6B;   // Boss1, Boss2, Boss3
}
inline void spawnBlood(double x, double y, int f, double dirX, double dirY, int count) {
    if (!faBlood()) return;
    int live = 0;                                              // кап: ≤16 частиц одновременно (ZT −$700a == 0x10)
    for (auto& a : actors()) if (a.active && a.think == AT_BLOOD) ++live;
    double dl = std::hypot(dirX, dirY); if (dl > 0.01) { dirX /= dl; dirY /= dl; } else { dirX = dirY = 0; }
    for (int i = 0; i < count && live < 16; ++i, ++live) {
        Actor& a = allocActor(); a.think = AT_BLOOD; a.x = x; a.y = y; a.floor = f; a.state = 0; a.timer = 0; a.frameT = 0;
        // ZT 0x157ca: vel $2a/$2c = d3/d4 + rnd((&0x7f)−0x40). d3/d4 = НОРМАЛИЗОВАННЫЙ вектор удара ×256 (receive 0x187e4:
        //   `<<8; divs dist`) → база = 1.0·unit кл/кадр = СТРУЯ по направлению удара (ДОМИНИРУЕТ), разброс = ±0x40/256 = ±0.25 кл.
        //   ⚠ Прежде база = 0.06 (в 16× слабее — кровь «пшикала» у врага; автор ошибочно счёл d3/d4 малым). z-off $24=rnd(−8..7), z-vel $2e=rnd(−9..6).
        a.vx = dirX * 1.0 + bloodRnd() * 0.25;                // СТРУЯ по направлению удара (256·unit /256) + разброс ±0.25
        a.vy = dirY * 1.0 + bloodRnd() * 0.25;
        a.z  = 0.5 + bloodRnd() * 0.12;                        // около центра тела ($24 −8..7 /64)
        a.vz = bloodRnd() * 0.12 - 0.02;                      // $2e −9..6 /64: чаще вверх, слегка вниз-смещён
        a.tile = A_BLOOD_FLY;
        a.variant = (bloodRnd() > 0.0) ? 1 : 0;              // ⭐ГОРИЗ.ФЛИП спрайта (ZT draw 15a2c/15a68/15a9e: $3e&1 → eda0 -$6f78) — разнообразие per-частица
    }
}
// ОРУЖИЕ, КОТОРОЕ РОНЯЕТ ВРАГ при смерти (ZT: «бывшие люди» — солдаты — роняют свой ствол, инопланетяне нет).
// ДРОП ОРУЖИЯ С ТРУПА — выверено по corpse-think дизасм (drop = object-cmd 0x13 → пикап-объект в 0x1c03a;
// 0x11084 = ЗАГРУЗКА СПРАЙТА пикапа в VRAM, НЕ «дать игроку» — прежняя трактовка была ошибочной):
//   • Former Human 0x2A (corpse-think 0x186b8, state3) = ВСЕГДА ПИСТОЛЕТ (id 8) [0x18742 move #$8,d0];
//   • FH-SF 0x69 (draw 0x19eca state3, 0x1a038) = RNG `btst #4`: laser(10) / grenade(7);
//   • FH-Sergeant 0x29 (think 0x1b078→морф 0x1b51a→draw 0x19eca) = как FH-SF: RNG laser/grenade;
//   • Revenant 0x66 (0x1af44, state $b) = ВСЕГДА laser(10);
//   • Imp/Hydaca/Dog/боссы — БЕЗ дропа (death только штампует труп cmd 0x12).
// Возврат: id оружия 1..14, или −2 = «RNG laser/grenade», или −1 = нет дропа. Пикап ВИДИМ на полу у трупа.
inline int enemyWeaponDrop(uint8_t ct) {
    switch (ct) {
        case 0x2A: return 8;    // Former Human → ПИСТОЛЕТ (всегда)
        case 0x66: return 10;   // Revenant → LASER AIMED GUN (всегда)
        case 0x29: return -2;   // FH-Sergeant → RNG бит4: laser(10) / grenade(7)
        case 0x69: return -2;   // FH-SF → RNG бит4: laser(10) / grenade(7)
        default:   return -1;   // Imp/Hydaca/Dog/боссы — без дропа
    }
}
// Труп врага (ZT 0x14a56: штампует corpse-celltype в карту). Порт: статичный актёр со спрайтом «лёжа».
// drop = id оброненного оружия (−1 нет) — подбирается шагом на труп (как ZT: бросает оружие у трупа).
// ОБРОНЕННОЕ ОРУЖИЕ (ROM corpse-think 186fa+): труп САМ носит оружие — НЕ роняет видимый пикап на пол
// (напольный объект-спавн cmd 0x13→1c03a = ТОЛЬКО 2P-link, ветка a3≠игрок @18774). Подбор — НАСТУПАНИЕМ:
// каждый кадр труп меряет дистанцию до игрока, при <0x60 (≈0.375 кл) и есть место → оружие в инвентарь +
// «COLLECTED» (jsr 11084); полный инвентарь (187a0=rts) → оружие ОСТАЁТСЯ в трупе (не теряется). См. corpsePickup.
inline void spawnCorpse(double x, double y, int f, uint8_t ct, double kvx = 0, double kvy = 0, uint8_t variant = 0, int drop = -1, bool burned = false) {
    Actor& a = allocActor(); a.think = AT_CORPSE; a.x = x; a.y = y; a.floor = f; a.srcType = ct; a.variant = variant;
    a.vx = kvx; a.vy = kvy; a.timer = 14;   // труп отлетает от смертельного удара и скользит ~14 кадров (трейс: 0.25-0.4 кл/кадр, затух.)
    a.drop = drop; a.burned = burned;       // сожжён огнём → обугленный труп
    a.frameT = 0; a.state = 0;              // frameT = тик анимации трупа (FH/Hydaca дёргаются); state = фаза $35 (простреленный кадр)
    snd::playSfx(snd::enemyDeathSfx(ct));   // ЗВУК смерти — УНИКАЛЕН по типу врага (enemy_ai.asm)
}
// Подбор оброненного оружия при шаге на труп: ищет труп под игроком с drop≥0, возвращает id (и снимает drop). cb добавляет в инвентарь.
template <typename AddFn>
inline void corpsePickup(const Camera& cam, AddFn add) {
    for (int pass = 0; pass < 2; ++pass)                 // трупы: свежие в пуле + эвиктированные в статик-мире
      for (auto& a : (pass ? staticActors() : actors())) {
        if (!a.active || a.think != AT_CORPSE || a.drop < 0 || a.floor != cam.floor) continue;
        if (std::hypot(cam.px - a.x, cam.py - a.y) < 0.375) { if (add(a.drop)) a.drop = -1; }  // ROM 0x60/256≈0.375 кл (18714 cmpi #$60); взял → снять
    }
}

// ── ДАННЫЕ ВРАГОВ (из objdef @0xAB0C, отчёт по дизасму) ─────────────────────────────────────────
// HP +0x12 (÷100 для играбельного масштаба): Sgt/FH/Revenant/dog 400→4, Imp 800→8, Hydaca 200→2,
// FH-SF 600→6, боссы 3000/8000/6000 → 30/80/60. (Враги-celltype: 0x29-0x2B, 0x65-0x6B.)
inline int enemyHp(uint8_t ct) {
    switch (ct) {
        case 0x2B: return 8;    // Imp
        case 0x65: return 2;    // Hydaca
        case 0x69: return 6;    // Former Human SF / Alien
        case 0x67: return 30;   // Boss 1
        case 0x6A: return 80;   // Boss 3
        case 0x6B: return 60;   // Boss 2
        default:   return 4;    // Sgt/FH/Revenant/dog
    }
}
// Тип атаки (по наблюдению юзера + дизасм 0x1b376 «direct damage» dist<0x400):
//  • БЛИЖНИЙ (Imp 0x2B, dog 0x68, Hydaca 0x65) — урон в УПОР (контакт, dist<0x64).
//  • ДАЛЬНИЙ-HITSCAN (Sgt 0x29, FH 0x2A, FH-SF 0x69, Revenant 0x66) — МГНОВЕННЫЙ урон по LOS, БЕЗ снаряда.
//  • БОСС (0x67/0x6A/0x6B) — ЕДИНСТВЕННЫЙ кто пускает ВИДИМЫЙ снаряд (AT_ENEMY_SHOT).
inline bool enemyMelee(uint8_t ct) { return ct == 0x2B || ct == 0x68 || ct == 0x65; }
inline bool enemyBoss(uint8_t ct)  { return ct == 0x67 || ct == 0x6A || ct == 0x6B; }

// ── КЛАССЫ ВРАГОВ (по разбору capstone их think-функций; см. enemy_ai.asm) ──
//  EC_RANGED      FH 0x2A/Sgt 0x29  — aim=игрок+скорость+rnd±1кл, hitscan <4кл (0x182fc/0x1b078)
//  EC_RANGED_FAST Revenant 0x66     — БЫСТРЫЙ (×2), hitscan <2кл, стреляет ~2× чаще, пак-агро (0x1a92a)
//  EC_GRENADIER   FH-SF 0x69        — как ranged + БРОСАЕТ ГРАНАТУ + открывает двери (0x19a48)
//  EC_MELEE       Imp 0x2B          — aim±0.5кл без упреждения, контакт <0.39кл, урон 12 (0x1887a)
//  EC_HOPPER      Hydaca 0x65       — прыгает (z-дуга), контакт <0.39кл урон 5, кулдаун (0x14c2a)
//  EC_DOG         Dog 0x68          — патруль→погоня(живой трек)→прыжок, контакт урон 6 + откид (0x19540)
//  EC_BOSS_PROJ   Boss1 0x67        — медленный, САМОНАВОДЯЩИЙСЯ снаряд урон 9 (0x19060→0x15f6e)
//  EC_BOSS_MELEE  Boss2 0x6B/Boss3 0x6A — быстрый чардж, ближний урон 9 (Boss3 close 12, enrage+revive)
enum EClass { EC_RANGED, EC_RANGED_FAST, EC_GRENADIER, EC_MELEE, EC_HOPPER, EC_DOG, EC_BOSS_PROJ, EC_BOSS_MELEE };
inline EClass enemyClass(uint8_t ct) {
    switch (ct) {
        case 0x66: return EC_RANGED_FAST;       // Revenant
        case 0x29: return EC_GRENADIER;         // Sgt — кидает гранаты (+ трансформируется в FH-SF по таймеру)
        case 0x69: return EC_GRENADIER;         // Former Human SF
        case 0x2B: return EC_MELEE;             // Imp
        case 0x65: return EC_HOPPER;            // Hydaca
        case 0x68: return EC_DOG;               // Pink dog
        case 0x67: return EC_BOSS_PROJ;         // Boss 1
        case 0x6A: case 0x6B: return EC_BOSS_MELEE;  // Boss 3 / Boss 2
        default:   return EC_RANGED;            // FH 0x2A / Sgt 0x29
    }
}
// УРОН игроку (ZT d800 @0xd864): dmg = 16 − (distUnits>>6), если distUnits<0x400, иначе 0. distUnits=distКл·256.
inline int zdamage(double distCells) {
    if (distCells >= 4.0) return 0;
    int dmg = 16 - (int)(distCells * 4.0);
    return dmg < 0 ? 0 : dmg;
}
// ИНДЕКС БОЙЦА U-RON ($FF1030 = -$6fd0): 0..4, влияет на ПЕРКИ оружия (дизасм 12eac/1303c/130de/11f3c).
// 0 = Ishii MARKSMAN (усилен. handgun). Меняется на брифинге (§9.5) — пока дефолт 0, ставится --fighter/консолью.
inline int& playerFighter() { static int f = 0; return f; }

// УРОН ОРУЖИЯ ИГРОКА врагу (ZT 187c8): HP−=(0x400−X), где X — дист-функция ствола (д7c0 дистанция). HP/урон в порте
// оба /100 → соотношение сохранено. ⭐ПЕРКИ БОЙЦА (verified 12eac/1303c/130de): фолофф X÷2 → дальнобойнее+сильнее:
//   handgun X=du²/512 (боец0/2 ÷2), laser X=du²/512, shotgun X=du (боец0 ÷2), pulse X=du/2 (боец0 ÷2), fire-ext X=0x3e8@<1.17кл.
inline int playerWeaponRawDamage(int id, double distCells) {
    double du = distCells * 256.0, X;
    int fg = playerFighter();
    switch (id) {
        case 8:  X = du * du / 512.0; if (fg == 0 || fg == 2) X *= 0.5; break;   // HANDGUN (боец0/2 фолофф÷2)
        case 10: X = du * du / 512.0; break;                                     // LASER GUN
        case 12: X = du;              if (fg == 0)            X *= 0.5; break;    // SHOTGUN (боец0 ÷2)
        case 14: X = du * 0.5;        if (fg == 0)            X *= 0.5; break;    // PULSE LASER (боец0 ÷2)
        case 4:  if (distCells > 1.17) return 0; X = 1000.0; break;              // FIRE EXT — фикс 0x3e8, дальность <0x12c
        default: X = du; break;                                                  // прочий hitscan ≈ shotgun
    }
    if (X >= 1024.0) return 0;                           // вне дальности (X ≥ 0x400)
    return (int)(1024.0 - X);
}
inline bool enemyLOS(const Level& lvl, int floor, double ex, double ey, double px, double py);  // fwd-декл (тело ниже)
// ⭐КОНУС-АВТОНАВЕДЕНИЕ (ZT 0x167b0, verified): БЛИЖАЙШИЙ живой враг на этаже в конусе оружия. По дизасму:
//   fwd = rel·dir (проекция на взгляд), scale = 64/fwd, lat = rel⊥dir; попадание если 64·|lat|/fwd ≤ scale/8 + d1.
//   d1 = полуширина конуса (handgun 1, laser 3, shotgun/pulse/foam 8; боец-перки шире). Берётся цель с МАКС scale
//   (ближайшая) среди попавших в конус. minScale>0 = порог ближней дальности (для мили-кулаков). Возврат idx или −1.
// ⭐СТОЙКА-МАСКА игрока (ROM 167ce, по питчу -0x71e6): прыжок(питч>0)→0x80, стойка(≈0)→0x40, присед(питч<0)→0x08.
// Актёр поражаем конусом ⟺ у него взведён бит текущей стойки (167e8: +0x4 & маска ≠ 0). Берём jump/crouch игрока
// (не cam.pitch — тот включает лифт/лестницу/нокбэк).
inline int playerStanceMask() {
    const PlayerState& p = player();
    if (p.jumpY   >  2.0) return 0x80;   // прыжок / целясь ВВЕРХ
    if (p.crouchY < -2.0) return 0x08;   // присед / целясь ВНИЗ
    return 0x40;                          // стойка
}
// ⭐БИТЫ СТОЙКИ актёра (ROM +0x4 = objdef+0x10, VERIFIED @0xAB0C): из какой стойки он поражаем.
inline int actorStanceBits(const Actor& a, double px, double py) {
    if (a.think == AT_CORPSE) return 0x08;                 // ТРУП: только присед (death andi #$ff2f → остаётся 0x08)
    if (a.think == AT_MINE)   return 0x08;                 // ⭐МИНА (спавн 12c36: flags|=0x8): прострелить можно ТОЛЬКО из приседа
    if (a.think != AT_ENEMY)  return 0xc8;                 // прочее (граната/ракета flags 0xC8: любая стойка) — не фильтруем
    if (a.srcType == 0x68)    return 0x48;                 // Pink Dog (objdef 0x5c): стоя+присед, НЕТ прыжка → перепрыгивается
    if (a.srcType == 0x65) {                               // Hydaca: поза по Z (пол↔потолок), маски think 14d4c/14cf6/14ca6
        if (std::fabs(a.vz) > 0.0001) return 0xc8;         // ПАДЕНИЕ (state2, 14ca6 ori #$C8) → любая стойка
        if (a.z < 0.25) return 0x48;                       // ПОЛ (14d56: andi ~0x80 | 0x40) → стоя+присед, НЕ прыжок
        if (a.z > 0.75) {                                  // ПОТОЛОК (14cf6: andi ~0x8 | 0x40) → стоя+прыжок, НЕ присед;
            // ⭐В УПОР (14d20: d7c0-дист ≤ 0x80 = 0.5 кл → andi #$ffb7 снимает 0x40 и 0x8): стоя ПОД НЕЙ
            // потолочную НЕ поразить — ТОЛЬКО В ПРЫЖКЕ. Отойди или прыгай (ROM-верно).
            return (gameDist(a.x - px, a.y - py) <= 0.5) ? 0x80 : 0xc0;
        }
        return 0xc8;                                       // лезет/между позами → все
    }
    return 0xc8;                                           // обычный враг (objdef 0xdc) → все стойки
}
inline int coneTargetEnemy(const Level& lvl, int floor, double px, double py, double dirX, double dirY,
                           int d1, int minScale = 0) {
    // БИТ-ТОЧНО как ZT 167b0 (integer 68k-ops): позиции ×256 (units), cos/sin амплитуда 256.
    long cs = std::lround(dirX * 256.0), sn = std::lround(dirY * 256.0);
    int best = -1, bestScale = minScale;                 // ZT держит МАКС scale (ближайшую цель)
    const int mask = playerStanceMask();                 // стойка игрока → какие актёры поражаемы
    auto& A = actors();
    for (int i = 0; i < (int)A.size(); ++i) {
        const Actor& a = A[i];
        bool liveEnemy = (a.think == AT_ENEMY && a.hp > 0);
        bool corpse    = (a.think == AT_CORPSE);           // ТРУП тоже цель конуса (поражаем из приседа) — ROM 167b0
        // ⭐ВЗРЫВЧАТКА тоже цель конуса (ROM 167b0 берёт ЛЮБОГО актёра с flags&маска): выстрел детонирует (receive 13ad2)
        bool explosive = (a.think == AT_MINE || a.think == AT_BULLET || a.think == AT_GRENADE);
        if (!a.active || a.floor != floor || (!liveEnemy && !corpse && !explosive)) continue;
        if ((actorStanceBits(a, px, py) & mask) == 0) continue;   // СТОЙКА-ФИЛЬТР (ROM 167e8): нет бита текущей стойки → пропуск
        long dx = std::lround((a.x - px) * 256.0), dy = std::lround((a.y - py) * 256.0);
        long d4 = (dx * cs + dy * sn) >> 6;              // forward_dot >> 6 (проекция на взгляд)
        if (d4 < 2) continue;                            // враг позади/впритык
        int scale = (int)(0x10000 / d4);                 // ZT scale = 0x10000/d4 (= 64/fwd_клетки)
        if (scale <= bestScale) continue;                // не ближе уже найденной (ZT bls skip)
        long ldot = dy * cs - dx * sn;                   // боковой dot
        int d6 = (int)(ldot / d4); if (d6 < 0) d6 = -d6; // |lateral/d4| (усечение как divs)
        int tol = (scale >> 3) + d1;                     // допуск = scale/8 + конус оружия
        if (d6 > tol) continue;                          // вне конуса
        if (!enemyLOS(lvl, floor, a.x, a.y, px, py)) continue;  // ZT ищет по ВИДИМЫМ → не бить сквозь стены
        best = i; bestScale = scale;
    }
    return best;
}
// ⭐DEPTH-ПОРОГ АВТОНАВЕДЕНИЯ (ZT 167b0 d0 = -$1e5a). Разобрано 2026-07-15: -$1e5a (0xFF61A6) — НЕ фикс.порог и НЕ
// пер-оружейный, а слот пер-колоночного depth-буфера растеризатора (0xFF6126..6226) = масштаб БЛИЖАЙШЕЙ СТЕНЫ в
// колонке прицела. Смысл: цель нельзя захватить ДАЛЬШЕ стены прямо перед прицелом (167b0 `cmp d0,d5; bls skip` —
// врага берём лишь если его scale > scale стены = он БЛИЖЕ стены). Трассируем луч взгляда до 1-й стены и считаем
// scale тем же целочисл. способом, что coneTargetEnemy для актёра. Нет стены → 0 (без нижнего предела, как boot-RAM=0).
// ⭐«НЕБЕСНЫЕ» СТЕНЫ ДЛЯ ПУЛЬ (ROM 13c36 луч-версия / 13cbc снаряд-версия, VERIFIED 2026-07-22):
// проверка по СЫРОМУ cell-ID (не celltype!) и ОБЛАСТИ уровня (-$58e4 = эпизод). Попадание в такую
// клетку = выстрел «в небо»: импакт-спрайт НЕ спавнится (12ad4), ракета исчезает БЕЗ взрыва (140f4),
// лазер-прицел и порог автонаведения смотрят СКВОЗЬ (прицел-буфер -$1eda пишут только грани стен,
// накрывающие ЦЕНТР экрана — низкие парапеты/окна его не трогают, ce2c..ce46).
// high = питч луча ≥ 0 (-$71e6; у снаряда — его Z $24 ≥ 0): из приседа камера НИЖЕ парапета →
// парапеты 03/A2/06..08 становятся твёрдыми (импакт есть). Наборы:
//  область 0 (станция): 01, 6E, 6F, 91..96 — ОКНА в космос (в картах эп1 массово 6E/6F);
//  область 1 (крыша):  всегда 01, 8F, A3; при high ещё 03, A2 (=ct7B граница карты!), 06..08 — парапеты;
//  область ≥2 (подвал): неба нет — импакт везде.
inline bool bulletSkyCellId(const Level& lvl, uint8_t id, bool high) {
    if (lvl.area == 0) return id == 0x01 || id == 0x6E || id == 0x6F || (id >= 0x91 && id <= 0x96);
    if (lvl.area == 1) {
        if (id == 0x01 || id == 0x8F || id == 0xA3) return true;
        if (!high) return false;                              // 13c9c: tst -$71e6; bmi → короткий список
        return id == 0x03 || id == 0xA2 || (id >= 0x06 && id <= 0x08);
    }
    return false;                                             // 13c56-ветка area≥2: return 0
}
// -$71e6 ≥ 0 (полный питч: присед/нокдаун < 0) — «стоя/в прыжке» для 13c36.
inline bool playerAimHigh() { return player().crouchY + player().knockPitch >= 0.0; }
inline int aimWallScale(const Level& lvl, int floor, double px, double py, double dirX, double dirY) {
    long cs = std::lround(dirX * 256.0), sn = std::lround(dirY * 256.0);
    const bool high = playerAimHigh();
    double x = px, y = py;
    for (int i = 0; i < 600; ++i) {                          // 31 кл / 0.06 ≈ 517 шагов (+запас), как traceMiss
        double nx = x + dirX * 0.06, ny = y + dirY * 0.06; int cx = (int)nx, cy = (int)ny;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return 0;          // край карты → без предела
        if (cellBlockedAt(lvl.cellType(floor, cx, cy), nx - cx, ny - cy) &&
            !bulletSkyCellId(lvl, lvl.cellId(floor, cx, cy), high)) {                // sky-стены НЕ в прицел-буфере (ce2c)
            long dx = std::lround((nx - px) * 256.0), dy = std::lround((ny - py) * 256.0);
            long d4 = (dx * cs + dy * sn) >> 6; if (d4 < 2) return 0;
            return (int)(0x10000 / d4);                                              // scale стены = порог (как ZT -$1e5a)
        }
        x = nx; y = ny;
    }
    return 0;
}
inline int playerWeaponDamage(int id, double distCells) {
    int raw = playerWeaponRawDamage(id, distCells);
    if (raw <= 0) return 0;                              // вне дальности
    int dmg = raw / 100;                                 // HP врага = ROM/100
    return dmg < 1 ? 1 : dmg;
}
// Параметры движения врага. ВЕРИФ. ПО ДИЗАСМУ: think вызывается КАЖДЫЙ кадр (планировщик 0x1327a: subq.b #1,$1f;
// bpl skip; think очищает $1f=0 → −1<0 каждый кадр), а move 0x145aa применяет ПОЛНУЮ скорость каждый think →
// per-frame: gain=1/2^shift, vmax=clampUnits/256 (БЕЗ деления на 7 — это была ошибка, враги были ×7 медленнее).
// РЕГУЛИРУЕМЫЙ множитель скорости врагов. ⭐2026-07-21: ROM-фактор «ход 3/4» (145aa: pos += vel − vel>>2)
// теперь ТОЧНЫЙ — ROM_WALK34 ниже; enemySpeedScale = чисто пользовательская ручка, дефолт 1.0.
// Прежний дефолт 0.8 был ПРИБЛИЖЕНИЕМ хода 3/4 — main мигрирует сохранённые 0.8 → 1.0 (иначе двойное замедление).
inline double& enemySpeedScale() { static double v = 1.0; return v; }
inline constexpr double ROM_WALK34 = 0.75;   // ZT 145aa: реальное смещение = vel·(1−1/4)
// ⭐МАСШТАБ ТИК-ДЛИТЕЛЬНОСТИ (enemyTimerScale в tuning.hpp): оборачивай КАЖДЫЙ тик-таймер врага `ticks(N)`.
inline int ticks(double n) {   // тик-длительность в КАДРАХ: ROM-тики / simDt (fps-инвариантно) × ручной масштаб
    double dt = simDt(); if (dt <= 0.0) dt = 1.0;
    int t = (int)(n * enemyTimerScale() / dt + 0.5); return t < 1 ? 1 : t;
}
struct EMove { double gain, vmax; };
inline EMove enemyMove(uint8_t ct) {
    double E = enemySpeedScale() * ROM_WALK34 * simDt();   // ⭐fps-инвариантно + ход 3/4 (ZT 145aa: vel−vel>>2)
    switch (enemyClass(ct)) {
        case EC_RANGED_FAST: return {1.0/2*2, 0x40/256.0*E};  // Revenant: >>1 ×2, ±0x40 (быстрейший дальний)
        case EC_GRENADIER:   return {1.0/8,   0x2d/256.0*E};  // FH-SF: >>3, ±0x2d
        case EC_MELEE:       return {1.0/4,   0x1e/256.0*E};  // Imp: >>2, ±0x1e
        case EC_HOPPER:      return {1.0/8,   0x28/256.0*E};  // Hydaca chase: >>3, ±0x28
        case EC_DOG:         return {1.0/2,   0x48/256.0*E};  // Dog ПОГОНЯ: >>1 (быстро!), ±0x48 (ZT 0x195ba asr#1 / 0x195d2 кламп 0x48). Патруль медленнее — через scale.
        case EC_BOSS_PROJ:   return {1.0/8,   0x0c/256.0*E};  // Boss1: >>3, ±0x0c (медленный)
        case EC_BOSS_MELEE:  return (ct == 0x6B) ? EMove{1.0/8, 0x10/256.0*E}   // Boss2: >>3, ±0x10 (медленный, ZT 0x18c98/0x18ca4)
                                                 : EMove{1.0/4, 0x20/256.0*E};  // Boss3: >>2, ±0x20 (быстрый чардж, ZT 0x1a422)
        default:             return {1.0/8,   0x28/256.0*E};  // FH/Sgt: >>3, ±0x28
    }
}
// Чит: враги ЗАМЕРЛИ на месте и не стреляют (для отладки), как в читах ZT.
inline bool& enemiesFrozen() { static bool v = false; return v; }
// ПОТОЛОК скорости VMAX (кл/кадр, пер-axis). ВЕРИФ. дизасм (0x183aa: clamp ±0x28=40 units) + трейс: think+move
// идёт ~раз в 7 кадров → эффективно ~40units/7 ≈ 0.022 кл/кадр. Беру чуть живее, тюнится по трейсу.
inline double enemySpeed(uint8_t ct) {
    switch (ct) {
        case 0x68: return 0.055;            // pink dog — самый быстрый
        case 0x2B: return 0.042;            // Imp (ближний)
        case 0x65: return 0.038;            // Hydaca (ближний)
        case 0x67: case 0x6A: case 0x6B: return 0.026;   // боссы медленнее
        default:   return 0.034;            // FH/Sgt/Revenant/FH-SF
    }
}
// HYDACA wall-check (ZT 0x150de/1512e/1517e/151ce → 0x1521e): стена ВПЛОТНУЮ = точка на 0x24 (36 units=0.14 кл)
// в сторону = стена (celltype LUT==1), И суб-позиция у соотв. КРАЯ клетки ($21/$23 ≤0x20=0.125). Возвращает направление
// стены (0=W 1=E 2=N 3=S) или −1. ⚠ Порт раньше брал nearWall (стена в любой соседней клетке) → Hydaca цеплялась
// ИЗДАЛЕКА (в центре клетки у стены-соседа). ZT цепляется лишь ВПЛОТНУЮ к грани.
// Стена, по которой Hydaca ЛАЗИТ (ZT wall-check 0x1521e: celltype LUT==1 = солидная стена). ⚠ ДИАГОНАЛИ (icon 2-5) ИСКЛЮЧЕНЫ
// (в ROM LUT≠1 → не лазит) — иначе Hydaca застревает в climb-петле у диагонали (юзер); двери тоже нет. Базовая/спец/разруш. стены.
inline bool hydClimbWall(uint8_t ct) { int ic = cellIcon(ct); return ic == 1 || ic == 12 || ic == 14; }
// ⭐ВЫБОР ПОЗЫ ЛАЗАНИЯ Hydaca (ZT draw 0x1539e states 3-A): per-state геометрия (dx=posX−camX восток+, dy=posY−camY юг+) →
// climbDir-индекс (поза a1=0,a2=1,a3=2,a4=3,a6=4,a7=5). Зависит от СТЕНЫ+ЛАТЕРАЛИ (state 3-10) и РАКУРСА к камере, НЕ только atan2.
inline int hydClimbPose(int state, double dx, double dy) {
    double adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    switch (state) {
        case 3:  if (dy <= 0 && adx < ady) return 3;  return dx > 0 ? 1 : 4;   // N→E: a4 / a2,a6
        case 4:  if (dy <= 0 && adx < ady) return 2;  return dx > 0 ? 0 : 5;   // N→W: a3 / a1,a7
        case 5:  if (dy >= 0 && adx < ady) return 2;  return dx > 0 ? 5 : 0;   // S→E: a3 / a7,a1
        case 6:  if (dy >= 0 && adx < ady) return 3;  return dx < 0 ? 1 : 4;   // S→W: a4 / a2,a6
        case 7:  if (dx >= 0 && ady < adx) return 2;  return dy < 0 ? 5 : 0;   // E→N: a3 / a7,a1
        case 8:  if (dx >= 0 && ady < adx) return 3;  return dy > 0 ? 1 : 4;   // E→S: a4 / a2,a6
        case 9:  if (dx <= 0 && ady < adx) return 3;  return dy < 0 ? 1 : 4;   // W→N: a4 / a2,a6
        case 10: if (dx <= 0 && ady < adx) return 2;  return dy > 0 ? 5 : 0;   // W→S: a3 / a7,a1
    }
    return 0;
}
inline int hydacaWallDir(const Level& lvl, int floor, double x, double y) {
    double fx = x - std::floor(x), fy = y - std::floor(y);       // суб-позиция в клетке 0..1
    const double R = 36.0 / 256.0;                                // 0x24 units
    if (fx <= 0.125 && hydClimbWall(lvl.cellType(floor, (int)std::floor(x - R), (int)y))) return 0;  // W
    if (fx >= 0.875 && hydClimbWall(lvl.cellType(floor, (int)std::floor(x + R), (int)y))) return 1;  // E
    if (fy <= 0.125 && hydClimbWall(lvl.cellType(floor, (int)x, (int)std::floor(y - R)))) return 2;  // N
    if (fy >= 0.875 && hydClimbWall(lvl.cellType(floor, (int)x, (int)std::floor(y + R)))) return 3;  // S
    return -1;
}
// PRNG для ИИ врага (как ZT jsr $a5c — случайный сдвиг точки прицела ±1 клетка). LCG, детерминир.
inline uint32_t enemyRng() { static uint32_t s = 0x13579bdfu; s = s * 1664525u + 1013904223u; return s; }
// СТЕЛС-ГЕЙТ ПОПАДАНИЯ стрелков (ZT 0x1846e/0x1b342/0x19cf2/0x1aa76/0x18dec/0x1a626 — идентичен у всех):
// порог d3 по СТОЙКЕ+СВЕТУ, rnd&0x3ff >= d3 → ПРОМАХ. стоя-свет 0x400 (всегда попал) / стоя-тьма 0x300 (25% промах) /
// присед-свет 0x200 (50%) / присед-тьма 0x17b (~63%). «Тьма» = тёмный режим этажа (ZT -$714e; порт: envMode≥2).
inline bool enemyShotMiss(int envMode) {
    bool crouch = (player().crouchY < -2.0);
    bool dark   = (envMode >= 2);
    int thr = crouch ? (dark ? 0x17b : 0x200) : (dark ? 0x300 : 0x400);
    return (int)(enemyRng() & 0x3ff) >= thr;
}
// Графика врага (объект-банк, ПЛЕЙСХОЛДЕР, по типу). Реальные спрайт-банки (0x1B7B38…, дерево 3 уровня
// 32×32 column-major, кадр 0x26 б) — отдельная graphics-задача (TODO).
inline uint8_t enemyTileForCt(uint8_t ct) {
    switch (ct) {
        case 0x29: return 22;   // Former Human Sergeant
        case 0x2A: return 24;   // Former Human
        case 0x2B: return 26;   // Imp
        case 0x65: return 28;   // Hydaca
        case 0x66: return 30;   // Revenant
        case 0x68: return 32;   // Pink dog
        case 0x69: return 34;   // Former Human SF
        case 0x67: return 36;   // Boss 1
        case 0x6A: return 38;   // Boss 3
        case 0x6B: return 40;   // Boss 2
        default:   return 30;
    }
}

// LOS врага→игрок: луч по прямой, блокируется стенами/диагоналями (как ZT 5-луч 0x166a0, упрощено до центра).
inline bool enemyLOS(const Level& lvl, int floor, double ex, double ey, double px, double py) {
    double dx = px - ex, dy = py - ey, d = std::hypot(dx, dy);
    if (d < 0.01) return true;
    dx /= d; dy /= d;
    for (double t = 0.2; t < d; t += 0.2) {
        double wx = ex + dx * t, wy = ey + dy * t;
        int cx = (int)wx, cy = (int)wy;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return false;
        uint8_t ct = lvl.cellType(floor, cx, cy);
        // ДИАГОНАЛЬ (0x02-0x05) = ТОНКАЯ ГИПОТЕНУЗА для зрения (ROM enemy-LOS 0166e8: полуплоскость по субкоордам
        // 16794/1677e/1678a/16772) — обзор проходит сквозь ПУСТУЮ половину клетки. Раньше cellBlocks квадратил →
        // враг «слеп» у диагонали (баг «детект у диагоналей»). Движение врагов ОСТАЁТСЯ квадратом (ROM 14858, faithful).
        if (ct >= 2 && ct <= 5) { if (cellBlockedAt(ct, wx - cx, wy - cy)) return false; }
        else if (cellBlocks(ct)) return false;                    // сплошная стена закрывает обзор
        if (cellIsDoor(ct) && doorOpen(floor, cx, cy) < 0.4) return false;  // ЗАКРЫТАЯ дверь блокирует; ОТКРЫТАЯ — пропускает
    }
    return true;
}

// Видимый снаряд врага. mode: 0=прямой, 1=САМОНАВОД (Boss1 0x15f6e — довод курса к игроку), 2=ГРАНАТА (FH-SF 0x13adc —
// летит по ДУГЕ: z вверх+гравитация, медленнее, крупнее). a.state=mode. Урон игроку по близости; у стены/двери — искра.
inline void spawnEnemyShot(double x, double y, int f, double dx, double dy, int mode = 0) {
    Actor& a = allocActor(); a.think = AT_ENEMY_SHOT; a.x = x; a.y = y; a.floor = f;
    a.state = mode;
    if (mode == 2) { a.vx = dx * 0.10 * simDt(); a.vy = dy * 0.10 * simDt(); a.z = 0.12; a.vz = 0.045;  // граната: дуга, медленнее
                     a.tile = A_EXPL_TILE; a.timer = 110; }
    else           { a.vx = dx * 0.25 * simDt(); a.vy = dy * 0.25 * simDt(); a.tile = A_SPARK_SEQ[0]; a.timer = 80;  // Boss1: прямой, ~0.25кл/кадр (ZT 0x192aa dx·64/dist)
                     a.z = 8.0; a.vz = (double)((int)(enemyRng() & 7) - 4); }  // ⭐Z-полоса снаряда (ROM 15f38: Z=8, zVel=(rnd&7)−4) — для верт.ДОЖА
}

// Создать ОДНОГО врага в клетке (x,y) этажа. ctOv≠0 — тип из маркера (эвиктированный враг мог стоять
// на «чужой» клетке). false = ПРОВАЛ alloc (пул полон, ROM 13466) — вызывающий ОСТАВЛЯЕТ маркер (ретрай).
inline bool spawnOneEnemy(const Level& lvl, int floor, int x, int y, uint8_t ctOv = 0) {
    uint8_t ct = ctOv ? ctOv : lvl.cellType(floor, x, y);
    Actor* ap = allocActorPtr(); if (!ap) return false;
    Actor& a = *ap; a.think = AT_ENEMY; a.floor = floor;
    a.x = x + 0.5; a.y = y + 0.5; a.hp = enemyHp(ct); a.srcType = ct; a.tile = enemyTileForCt(ct);
    a.homeX = a.x; a.homeY = a.y;                                          // дом = точка спавна
    a.state = 0; a.timer = 0; a.fireCd = 20 + ((x * 7 + y * 13) % 40);
    // Sgt (0x29): таймер трансформации в FH-SF (ZT state6 0x1b51a — морф человека в инопланетянина по истечении времени).
    a.xformT = (ct == 0x29) ? ticks(50 + ((x * 11 + y * 17) % 32)) : 0;    // ⭐ZT $41=0x32+rnd&0x1f=50-81 тик (~3.3-5.4с @15fps); ×enemyTimerScale (настройка)
    a.variant = (ct == 0x2A) ? (uint8_t)((x * 5 + y * 3) & 1) : 0;         // только FH: 2 визуальные вариации (Hydaca драйвит variant по z)
    if (ct == 0x65) { bool onCeil = ((x * 5 + y * 3) & 1); a.z = onCeil ? 1.0 : 0.0; a.variant = onCeil ? 1 : 0; }  // Hydaca: ПОЛ/ПОТОЛОК 50/50 (ZT init $34=rnd&1)
    { int as = snd::enemyAppearSfx(ct); if (as >= 0) snd::playSfx(as); }  // ЗВУК ПОЯВЛЕНИЯ врага (подтверждён на слух в оригинале)
    return true;
}
// Спавн врага ЗАДАННОГО типа в точке (для консоли). ct — celltype врага (0x29..0x6b).
inline void spawnEnemyByType(int floor, double x, double y, uint8_t ct) {
    Actor& a = allocActor(); a.think = AT_ENEMY; a.floor = floor;
    a.x = x; a.y = y; a.hp = enemyHp(ct); a.srcType = ct; a.tile = enemyTileForCt(ct);
    a.homeX = x; a.homeY = y; a.state = 0; a.timer = 0; a.fireCd = 20;
    a.xformT = (ct == 0x29) ? 300 : 0;
    if (ct == 0x65) { bool onCeil = (((int)x * 5 + (int)y * 3) & 1); a.z = onCeil ? 1.0 : 0.0; a.variant = onCeil ? 1 : 0; }  // пол/потолок 50/50
    { int as = snd::enemyAppearSfx(ct); if (as >= 0) snd::playSfx(as); }  // ЗВУК ПОЯВЛЕНИЯ (objdef+0x25)
}
// ПРОКСИ-СПАВН (ZT 0x15d18: скан 11×11 ±5 кл вокруг игрока — враги ПОЯВЛЯЮТСЯ когда подходишь, не все при загрузке!).
// Маркеры этажа собираются в pending; updateEnemySpawns каждый кадр спавнит те, что в радиусе. Это убирает «враги
// активны/несутся со старта по всей карте» — они материализуются по мере продвижения, как в оригинале.
// ⭐ПЕРСИСТЕНТНОСТЬ ЭТАЖЕЙ (ROM b8fc, 2026-07-16): смена этажа НЕ переинициализирует мир — грид каждого
// этажа живёт в RAM постоянно ($FF7CD6+floor*0x400, 16 гридов), пул актёров ГЛОБАЛЬНЫЙ (чистится только
// на загрузке уровня, 133a4). Маркеры несут ЭТАЖ; collect вызывается ОДИН РАЗ на первый визит этажа.
// ct: тип врага маркера. 0 = прочитать из грида (первичный маркер уровня); ≠0 = ЭВИКТИРОВАННЫЙ враг
// (ROM 14a56: даль ≥10 кл → актёр штампуется обратно в грид спящим маркером СВОЕГО ct (+0x46);
// состояние HP/морфа ТЕРЯЕТСЯ — разбуженный заново враг свежий, ROM-верно).
struct PendingSpawn { int x, y, floor; uint8_t ct = 0; };
inline std::vector<PendingSpawn>& pendingSpawns() { static std::vector<PendingSpawn> v; return v; }
inline int pendingOnFloor(int floor) {
    int n = 0; for (auto& m : pendingSpawns()) if (m.floor == floor) ++n; return n;
}
inline void collectEnemyMarkers(const Level& lvl, int floor) {        // ПЕРВЫЙ визит этажа: собрать маркеры (append, НЕ спавнить)
    auto& p = pendingSpawns();
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (cellIcon(lvl.cellType(floor, x, y)) == 9) p.push_back({x, y, floor, lvl.cellType(floor, x, y)});
}
inline void clearFloorState() { pendingSpawns().clear(); }            // полный сброс (смена уровня/эпизода)
// КАРТА-ОГОНЬ: клетки celltype 0x18 (Flame) → вечный AT_FIRE (хазард). Вызывать при загрузке/смене этажа.
inline std::unordered_set<int>& fireExtinguished() { static std::unordered_set<int> s; return s; }  // потушенные клетки (floor*1024+y*32+x)
inline void spawnMapFires(const Level& lvl, int floor) {
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (lvl.cellType(floor, x, y) == 0x18) {
                if (fireExtinguished().count(floor * 1024 + y * 32 + x)) continue;   // потушено игроком — не возрождать
                // ⭐СТАТИК-МИР, не пул (ROM: огонь 0x18 = грид-клетка с анимацией, актёром не является):
                // вечные огни копятся со всех посещённых этажей и забивали пул (стирание врагов при переполнении)
                bool dup = false;
                for (auto& e : staticActors()) if (e.active && e.think == AT_FIRE && e.floor == floor &&
                                                   (int)e.x == x && (int)e.y == y) { dup = true; break; }
                if (dup) continue;
                Actor a{}; a.active = true; a.think = AT_FIRE; a.x = x + 0.5; a.y = y + 0.5; a.floor = floor;
                a.tile = A_FIRE_TILE; a.timer = -1; a.frameT = (x * 7 + y * 13) & 7; a.state = 0;   // state0 = хазард (жжёт step-on)
                staticActors().push_back(a);
            }
}
// ⭐STEP-ON УРОН ОГНЁМ ИГРОКУ (ZT e662 через e3f8): срабатывает ПРИ СМЕНЕ КЛЕТКИ (не per-frame — ROM e88e сравнивает
// клетку под игроком с прошлой -0x71ea). Игрок ВОШЁЛ в клетку карта-огня 0x18 → РАЗОВЫЙ урон = 16−(dist>>6) (ZT d800),
// где dist = смещение от центра клетки в юнитах (256/кл). Костюм (fireImmune) гасит (ROM d7dc→10f98 расход костюма, без HP).
// ⚠НЕ радиус вокруг очага (был баг 0.19) и НЕ периодический — проходишь огонь = теряешь ~14-16 HP за клетку, стоишь = не жжёт.
inline int& suitBurnAbsorbs() { static int n = 0; return n; }   // поглощённые костюмом ожоги (main списывает заряд id5)
// ⭐КОНЕЦ ЭПИЗОДА (ROM: смерть БОССА 0x67/0x6A/0x6B → -$58dc=0xF; гл.цикл 1728 дотикивает → -$58de=1 →
// выход из геймплея на 1a40: слайд-заставка → эпизод+1 → пароль → загрузка нового эпизода; после 3-го → финал).
// Порт: смерть босса ставит episodeEndT=15; main разруливает заставки/переход. Boss3 (0x6A) триггерит только
// НАСТОЯЩУЮ смерть (притворство state99/воскрешение не проходит через death-путь).
inline int& episodeEndT() { static int t = 0; return t; }
inline void updatePlayerStepOnFire(const Camera& cam) {
    PlayerState& p = player();
    int cx = (int)cam.px, cy = (int)cam.py;
    int key = cam.floor * 1024 + cy * 32 + cx;
    if (key == p.lastCellKey) return;                    // клетка не сменилась → step-on не триггерится (ZT e88e)
    p.lastCellKey = key;
    for (int pass = 0; pass < 2; ++pass)                 // карта-огни живут в СТАТИК-мире (грид-клетки ROM), пул — на всякий
      for (auto& e : (pass ? staticActors() : actors()))
        if (e.active && e.think == AT_FIRE && e.state == 0 && e.floor == cam.floor &&
            (int)e.x == cx && (int)e.y == cy) {          // вошёл в клетку карта-огня
        if (p.fireImmune) { ++suitBurnAbsorbs(); return; }   // костюм гасит ожог; расход 1.0 спишет main (ROM d7dc→10f98)
        int distU = (int)(std::hypot(cam.px - e.x, cam.py - e.y) * 256.0);    // смещение от центра клетки (юниты)
        int dmg = 16 - (distU >> 6); if (dmg < 1) dmg = 1;                    // ZT d800: 16 − (dist>>6)
        damagePlayer(dmg, cam.px, cam.py, e.x, e.y);
        return;
    }
}
// «НЕУБИТЫЕ ВРАГИ» для счётчика кокпита (ZT 57e0c): живые актёры-враги этажа + ещё НЕ РАЗБУЖЕННЫЕ маркеры
// в гриде (спящие ct 0x29-0x2B/0x65-0x6B, у нас = pendingSpawns). Кламп 99 (2 цифры, ZT 1e00e).
inline int enemyCountRemaining(int floor) {
    int n = aliveEnemies(floor) + pendingOnFloor(floor);              // маркеры ТОЛЬКО этого этажа (грид этажа, ROM 57e5c)
    return n > 99 ? 99 : n;
}
// ⭐ЭТАЖ ПРОГРЕССА (ROM -$58D6): двигается только через ЗАЧИСТКУ (b8fc→57ec4). Счётчик врагов кокпита и
// пересчёт SECURED (57e0c) считают ИМЕННО его, не текущий этаж камеры — спуск без зачистки оставляет
// счётчик на прежнем этаже (ROM-верно). Обновляет main; renderReference читает для HUD.
inline int& pwProgressFloor() { static int f = 0; return f; }
// ⭐ДАЛЬНОСТЬ ОБЗОРА/АКТИВАЦИИ по освещению (ROM -$714c, сеттеры 1d66/1d46): Bright=0x10=16 клеток,
// No-ceiling=0xC=12, Dim/Haze/Black=5; ФОНАРЬ или НОЧНИК форсят 16 (1d5e/11286) — свет ВЫДАЁТ игрока:
// в темноте враги активируются с 5 клеток, со включённым фонарём — с 16 («враги реагируют на фонарик»).
inline double lightRange(int env) {
    if (nvActive() || flActive()) return 16.0;
    if (env == 0) return 16.0;                       // Bright
    if (env == 3) return 12.0;                       // No-ceiling
    return 5.0;                                      // Dim/Haze/Black
}
inline void updateEnemySpawns(const Level& lvl, int floor, double px, double py, double range = 0.0) {
    // ⭐ПРОБУЖДЕНИЕ по ROM (рендер-wake 9a6a: спящий маркер-СТАТУЯ оживает при dist<0x800 = 8 кл):
    // кап 8 кл; в темноте окно обзора -$714c (5 кл) режет раньше — фонарь/ночник поднимают до капа.
    // Статуи маркеров этажа рисуются в updateActors → враги больше не «появляются из ниоткуда».
    if (range <= 0.0) range = std::min(8.0, lightRange(lvl.env(floor)));
    auto& p = pendingSpawns();
    for (size_t i = 0; i < p.size(); ) {                              // спавн маркеров в радиусе (box ±range) от игрока
        if (p[i].floor != floor) { ++i; continue; }                   // маркеры других этажей спят (прокси 11×11 = этаж игрока)
        double mx = p[i].x + 0.5, my = p[i].y + 0.5;
        // LOS-гейт: не спавнить СКВОЗЬ стену/ЗАКРЫТУЮ дверь (юзер: «триггерятся до того как дверь открыл»)
        if (std::abs(px - mx) <= range && std::abs(py - my) <= range && enemyLOS(lvl, floor, mx, my, px, py) &&
            spawnOneEnemy(lvl, floor, p[i].x, p[i].y, p[i].ct)) {   // провал alloc (ROM 13466) → маркер ОСТАЁТСЯ, ретрай
            p[i] = p.back(); p.pop_back();
        } else ++i;
    }
}
// Враги ОТКРЫВАЮТ дверь, на/у которой стоят (чтобы проходить через двери, как игрок — а не клипать закрытую).
// Враги, ОТКРЫВАЮЩИЕ двери (ZT 5 зовов b1c4/b202): Sgt 0x29, FH-SF 0x69, Boss1 0x67, Boss2 0x6B, Boss3 0x6A.
// FH 0x2A, Imp 0x2B, Hydaca 0x65, Revenant 0x66, Dog 0x68 — НЕ открывают (закрытая дверь их блокирует).
inline bool enemyOpensDoors(uint8_t ct) {
    return ct == 0x29 || ct == 0x67 || ct == 0x69 || ct == 0x6A || ct == 0x6B;
}
inline void openDoorsAtEnemies(const Level& lvl, int floor) {
    auto& m = doorMap();
    for (auto& a : actors()) {
        if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
        int cx = (int)a.x, cy = (int)a.y;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) continue;
        if (!cellIsDoor(lvl.cellType(floor, cx, cy))) continue;
        int k = doorKey(floor, cx, cy);
        double prev = (m.count(k) ? m[k] : 0.0);
        double o = prev + 0.20; if (o > 1) o = 1; m[k] = o;
        if (prev <= 0.0) snd::playSfx(0x67);   // ⭐ЗВУК ДВЕРИ 0x67 при НАЧАЛЕ открытия врагом (ZT b1c4-путь; было беззвучно)
    }
}
// Спавн ВСЕХ врагов сразу (для дампа/скриншота — не геймплей).
inline void spawnEnemiesFromLevel(const Level& lvl, int floor) {
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (cellIcon(lvl.cellType(floor, x, y)) == 9) spawnOneEnemy(lvl, floor, x, y);
}

// ── КАМЕРА-ТРЕВОГА (celltype 0x26; ZT actor think 15b1e, детект 15bc2, тревога 15b52) ──────────────
// ЕДИНСТВЕННЫЙ легитимный источник разрушения стен/перманентного открытия дверей в игре.
// Детект (state0): игрок ≤0x300 (3кл; боец4 ≤0x180=1.5кл) + LOS(166a0) → взвод $35=0x21 (33 тика; боец4 0x31=49), $34=1.
// Взвод (state1): −1/тик, писк 0x71 каждые 8 тиков; $35→0 → ТРЕВОГА: 15c74 открыть/разрушить двери+стены 11×11
// (b130/b168 → пусто НАВСЕГДА) + 15d18 разбудить врагов 11×11 + сирена 0x70/дверь 0x67. Одноразово (state2).
// Простреливание (hHit 15e4e): 1 HP → взрыв 0x8A + падение с потолка, тревога НЕ срабатывает (destroyAlarmCam).
// Рендер: реальный мигающий спрайт камеры (draw 15d84, tile 19/20 банка объектов) — см. pushCameraBillboards.
struct AlarmCam { int x, y, floor; int state = 0; int timer = 0; bool dead = false; };
inline std::vector<AlarmCam>& alarmCams() { static std::vector<AlarmCam> v; return v; }
inline void collectAlarmCams(const Level& lvl, int floor) {           // ПЕРВЫЙ визит этажа: собрать камеры 0x26 (append)
    auto& v = alarmCams();
    for (auto& c : v) if (c.floor == floor) return;                   // этаж уже собран (состояние камер персистентно)
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (lvl.cellType(floor, x, y) == 0x26) v.push_back({x, y, floor, 0, 0, false});
}
// ЖИВАЯ камера в клетке (для хитскана/взрыва) → индекс или −1.
inline int liveAlarmCamAt(int floor, int cx, int cy) {
    auto& v = alarmCams();
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].floor == floor && !v[i].dead && v[i].x == cx && v[i].y == cy) return (int)i;
    return -1;
}
inline bool alarmCamDeadAt(int floor, int cx, int cy) {              // для рендера: скрыть СБИТУЮ камеру
    for (auto& c : alarmCams()) if (c.floor == floor && c.x == cx && c.y == cy) return c.dead;
    return false;
}
// СБИТЬ камеру (ZT hHit 15e4e: 1 хит → взрыв 0x8A + падение, тревога НЕ срабатывает).
inline void destroyAlarmCam(int idx) {
    auto& c = alarmCams()[idx];
    c.dead = true; c.state = 2;                                      // мертва + «сработавшей» (updateAlarmCams пропустит)
    spawnExplosionA(c.x + 0.5, c.y + 0.5, c.floor, 0.7);            // взрыв на высоте камеры
    snd::ev(snd::SFX_EXPLOSION);
}
// ТРЕВОГА (ZT 15b52): открыть/разрушить двери+стены 11×11 (15c74) + разбудить врагов 11×11 (15d18).
inline void triggerAlarm(const Level& lvl, int floor, int cx, int cy, double px, double py) {
    for (int dy = -5; dy <= 5; ++dy)                                  // 15c74: скан 11×11 (±5)
        for (int dx = -5; dx <= 5; ++dx) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= Level::W || y >= Level::H) continue;
            uint8_t ct = lvl.cellType(floor, x, y);
            if (ct == 0x06 || ct == 0x07 || ct == 0x83 || ct == 0x84) // двери 06/07 (b130/b168) + стены 83/84
                requestDestruct(floor, x, y);                         // → 0x2D/0x2E → пусто НАВСЕГДА (applyDestruct)
        }
    auto& p = pendingSpawns();                                        // 15d18: разбудить/заспавнить спящих врагов 11×11
    for (size_t i = 0; i < p.size();) {
        // ⚠ФИЛЬТР ЭТАЖА обязателен (persist-баг 2026-07-16): маркеры ВСЕХ посещённых этажей в одном списке —
        // без фильтра тревога спавнила врагов ЧУЖИХ этажей по совпадению координат («стреляющие спрайты» эп2 эт4)
        if (p[i].floor == floor && std::abs(p[i].x - cx) <= 5 && std::abs(p[i].y - cy) <= 5 &&
            spawnOneEnemy(lvl, floor, p[i].x, p[i].y, p[i].ct)) {   // провал alloc → маркер остаётся
            p[i] = p.back(); p.pop_back();
        } else ++i;
    }
    for (auto& a : actors()) {                                        // все враги радиуса → бегут к игроку (тревога = знают где ты)
        if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
        if (std::abs((int)a.x - cx) <= 5 && std::abs((int)a.y - cy) <= 5 && a.state == 0) {
            a.state = 1; a.aimX = px; a.aimY = py;
        }
    }
    snd::playSfx(0x70);                                               // сирена тревоги
    snd::playSfx(0x67);                                               // звук двери
}
// ⭐СТРОГИЙ 5-ЛУЧЕВОЙ LOS камеры (ROM 166a0): камера видит игрока ⟺ ВСЕ 5 лучей чисты — центр + смещения ТОЧКИ
// КАМЕРЫ на ±0x1c (±0.109 кл) по X и по Y. Строже одиночного луча: убирает срабатывания из-за угла/сбоку (fidelity;
// каждый луч = enemyLOS = порт-версия ROM-луча 166e8: cell-march, блок на стенах/закрытых дверях, диагонали полуплоскостью).
inline bool cameraLOS(const Level& lvl, int floor, double cx, double cy, double px, double py) {
    const double o = 0x1c / 256.0;                                   // 0.109375 кл (ROM ±0x1c к endpoint камеры)
    return enemyLOS(lvl, floor, cx,     cy,     px, py)
        && enemyLOS(lvl, floor, cx + o, cy,     px, py)
        && enemyLOS(lvl, floor, cx - o, cy,     px, py)
        && enemyLOS(lvl, floor, cx,     cy + o, px, py)
        && enemyLOS(lvl, floor, cx,     cy - o, px, py);
}
inline void updateAlarmCams(const Level& lvl, int floor, double px, double py) {
    int fg = playerFighter();
    double range = (fg == 4) ? (0x180 / 256.0) : (0x300 / 256.0);     // боец4 (стелс) детектится ближе (1.5кл), иначе 3кл
    for (auto& c : alarmCams()) {
        if (c.floor != floor || c.state == 2) continue;              // state2 = уже сработала (одноразово)
        double dist = gameDist(px - (c.x + 0.5), py - (c.y + 0.5));  // октаг. d7c0
        if (c.state == 0) {                                          // ДЕТЕКТ: дистанция + строгий 5-лучевой LOS (166a0)
            if (dist <= range && cameraLOS(lvl, floor, c.x + 0.5, c.y + 0.5, px, py)) {
                c.state = 1; c.timer = (fg == 4) ? 0x31 : 0x21;      // ВЗВОД: 49/33 тика
            }
        } else {                                                     // state1: ВЗВОД
            if (--c.timer <= 0) { triggerAlarm(lvl, floor, c.x, c.y, px, py); c.state = 2; }
            else if ((c.timer & 7) == 0) snd::playSfx(0x71);         // ПИСК каждые 8 тиков
        }
    }
}
// РЕНДЕР камер: пушим ЖИВЫЕ камеры этажа в worldFx как высоко-настенные билборды (сбитые — не пушим → исчезают).
// РЕАЛЬНЫЙ спрайт (draw 15d84, eda0-билборд как декор): камера безопасности с МИГАЮЩИМ объективом —
// банк объектов 0x10E9BE, tile 19 = 0x110FBE (диод ВЫКЛ) ↔ tile 20 = 0x1111BE (диод ВКЛ), чередуются по
// `btst #1, -$714a` (бит1 глобального тик-счётчика = decorFrame) → блинк с периодом 4 тика. Мигание НЕ зависит
// от состояния тревоги (draw не меняется между детект/взвод/выстрел — камера всегда мигает; только выстрел → взрыв).
// Геометрия из draw-математики 15d84: −0x20 в pitch-терме → topOff16=−S/2=−8 (верх у потолка); высота 3·S/8 → hFrac16=6.
// Ширина: eda0 d4=3·S/16 в 128-колоночном пространстве райкастера, а дисплей растягивает горизонталь ×2 (128 лучей→256)
// → на экране ширина = 3·S/8 = высота → спрайт КВАДРАТНЫЙ (тайл 32×32). worldFx-путь square=true считает width от S (без
// ×2 декора-cwd), поэтому wFrac16=6 (= hFrac16), а НЕ 3 (иначе вдвое узко — как старый баг спрайтов 1:2). Шейдинг/z-тест — общий (eda0 CLUT).
inline void pushCameraBillboards(int floor) {
    const uint8_t tile = ((decorFrame() >> 1) & 1) ? 20 : 19;            // 15d84: btst #1 тик-счётчика → 19↔20
    for (auto& c : alarmCams()) {
        if (c.floor != floor || c.dead) continue;
        WorldFx fx; fx.wx = c.x + 0.5; fx.wy = c.y + 0.5; fx.floor = floor;
        fx.tile = tile; fx.topOff16 = -8; fx.hFrac16 = 6; fx.wFrac16 = 6;  // потолочная камера безопасности (eda0 0x110FBE/0x1111BE), квадрат 32×32
        worldFx().push_back(fx);
    }
}

// УРОН ВРАГУ (ZT damage-handler 0x1B824): HP−=dmg; KNOCKBACK (отлёт ОТ источника, ∝ урону) + стаггер +
// искра-импакт; смерть HP≤0 → ТРУП (не взрыв). fromX/fromY = источник урона (для направления отлёта).
// ZT receive-handler (sub_0187c8): на КАЖДОМ попадании ставит state=2 (СТАГГЕР), обнуляет+задаёт нокбэк-вектор
// v=unit(от источника)·(урон>>3), вычитает HP (может уйти В МИНУС). СМЕРТЬ НЕ мгновенная — труп появляется в think
// ТОЛЬКО когда нокбэк-скорость ЗАТУХАЕТ ниже 0x0a И HP<0 (sub_01887a $188b2: tst $36; bmi death). Поэтому ПОКА
// СТРЕЛЯЕШЬ — каждый выстрел освежает скорость → враг ДЕРЖИТСЯ в стаггере; перестал → скорость села → труп.
inline void hitEnemy(Actor& a, int dmg, double fromX, double fromY, int romDmg = -1) {
    if (romDmg < 0) romDmg = dmg * 100;                                   // реконструкция ROM-урона (порт-урон = ROM/100)
    a.hp -= dmg;
    double dx = a.x - fromX, dy = a.y - fromY, d = std::hypot(dx, dy);    // отлёт ОТ источника
    // КРОВЬ-ВСПЛЕСК (ZT hHit 0x187c8→0x157ca): брызги ТОЛЬКО на СМЕРТЕЛЬНОМ ударе (killing blow) с уроном ≥0x338 —
    // дизасм: `tst $36; bmi skip; sub d0,$36; bpl skip` = splat лишь когда HP переходит ≥0→<0 (НЕ живые удары, НЕ труп).
    // Постоянная «кровь трупа» — это АНИМАЦИЯ СПРАЙТА (напр. FH модель B, кадры a4 с брызгами), а не частицы.
    bool killedNow = (a.hp <= 0 && a.hp + dmg > 0);                       // этот удар убил (был жив, стал мёртв)
    if (enemyBleeds(a.srcType) && romDmg >= 0x338 && killedNow) {
        int n = bloodParticleCount(romDmg);
        if (n > 0) spawnBlood(a.x, a.y, a.floor, dx, dy, n);
    }
    // СМЕРТЕЛЬНЫЙ ФОНТАН БОССОВ: на УБОЙНОМ ударе (HP из ≥0 в <0) — двойной вызов d0=0x400 (ZT receive: 0x194da/e2, 0x18ffa/02,
    // 0x1a8a6/ae), ~10 частиц. БЕЗ гейта ≥0x338 (форс d0=0x400). Гейт killedNow — НЕ повторять на трупе (ZT: `bmi` если HP уже <0).
    if (killedNow && enemyDeathGush(a.srcType)) {
        int n = bloodParticleCount(0x400);
        spawnBlood(a.x, a.y, a.floor, dx, dy, n); spawnBlood(a.x, a.y, a.floor, dx, dy, n);
    }
    if (a.hp <= 0 && a.think == AT_ENEMY && a.srcType == 0x6A && !a.revived) {  // BOSS3: первая «смерть» → ПРИТВОРЯЕТСЯ МЁРТВЫМ
        a.state = 99; a.timer = ticks(25); a.vx = a.vy = 0; a.hitT = 0; a.fireAnimT = 0;  // ZT 0x1a3aa: state6, лежит 25 тиков → воскресает ×enemyTimerScale
        return;
    }
    // ⭐HYDACA (ZT hHit 0x1575c): попадание СБИВАЕТ в state2 (z-vel=0 → ПАДЕНИЕ гравитацией), НЕ обычный xy-стаггер.
    // На ПОЛУ ($24=−24, z≈0) → z-vel=+8 = ПОДПРЫГ вверх. Иначе (потолок/висит) → падает вниз. Горизонт гасится (ZT asr).
    if (a.think == AT_ENEMY && a.srcType == 0x65) {
        a.state = 1;                                              // активна, БАЛЛИСТИКА (не climb=state2)
        if (a.z <= 0.05) a.vz = 8.0 / 48.0;                       // на ПОЛУ → ПОДПРЫГ вверх (ZT 0x1575c: z-vel=+8; шкала 48 units)
        else if (a.vz >= 0.0) a.vz = -0.0001;                     // на ПОТОЛКЕ/висит → начать ПАДАТЬ из покоя (ZT z-vel=0, гравитация разгонит)
        a.vx = a.vy = 0; a.hitT = 0;                              // без xy-скольжения — движение чисто вертикальное (ZT гасит горизонт)
        return;                                                   // (смерть решится при приземлении по HP)
    }
    // REVENANT (ZT hHit→стаггер state A→recover 1abd8→re-engage 1abe0): подстреленный ВСТУПАЕТ В БОЙ (даже издалека),
    // НО пак-алерт (1ab6c) НЕ зовёт → соседей-патрульных не будит (можно снять одного снайпингом, группа спит).
    if (a.think == AT_ENEMY && a.srcType == 0x66 && a.state < 8) a.state = 1;  // → revEngage в combat-блоке
    // НОКБЭК ZT: v = (вектор от источника)·(урон>>3). Порт-урон = ROM/100 → v ≈ dmg·0.049 кл/кадр; затух. ÷2/кадр.
    double k = dmg * 0.049; if (k > 0.55) k = 0.55;
    if (d > 0.01) { a.vx = dx / d * k; a.vy = dy / d * k; } else { a.vx = a.vy = 0; }
    if (a.hitT <= 0) a.hitT = 1;                                          // ВХОД в стаггер (выход — по затуханию v, не таймеру)
    // (НЕТ импакт-искры по врагу: искра 0x12ad4 — только промах в СТЕНУ; по врагу = стаггер-аним/кровь)
}

// ⭐АНИМАЦИЯ ХОДЬБЫ = ДИСТАНС-АККУМУЛЯТОР (ZT 1ba04 @1bb6a, VERIFIED 2026-07-21): суб-кадр ходьбы НЕ по времени!
// d2=-1: скорость=d7c0(|vx|,|vy|); <4 ед (=4/256 кл/тик) → СТОЯЧИЙ суб-кадр 0; иначе $3c+=скорость,
// индекс=($3c>>3)&31 → ПАТТЕРН-ТАБЛИЦА @1bbaa[(N-1)*32] (N=число ходячих суб-кадров, значения 1..N).
// Частота шагов ∝ пройденному пути («шаги по земле»), стоящий враг всегда в стоячей позе.
// Таблица снята байт-в-байт из ROM 0x1bbaa (8 строк × 32).
inline const uint8_t WALK_PAT[8][32] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,2,2,2,2,1,1,1,1,2,2,2,2,1,1,1,1,2,2,2,2,1,1,1,1,2,2,2,2},
    {1,1,1,1,2,2,2,3,3,3,3,1,1,1,2,2,2,2,3,3,3,1,1,1,1,2,2,2,3,3,3,3},
    {1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4},
    {1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,1,1,1,2,2,2,3,3,3,3,4,4,4,5,5,5},
    {1,1,1,1,1,1,2,2,2,2,2,3,3,3,3,3,4,4,4,4,4,4,5,5,5,5,5,6,6,6,6,6},
    {1,1,1,1,1,2,2,2,2,3,3,3,3,3,4,4,4,4,5,5,5,5,5,6,6,6,6,7,7,7,7,7},
    {1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,8,8}};
// Суб-кадр ходьбы по ROM-модели: nf = ВСЕ суб-кадры (0=стоячий + 1..N ходьба). Вызывать 1×/тик (аккумулирует).
inline uint8_t walkFrame(Actor& a, int nf) {
    if (nf <= 1) return 0;
    int spd = (int)std::lround(gameDist(a.vx, a.vy) * 256.0);   // октаг. d7c0, ед. 256/кл
    if (spd < 4) return 0;                                      // стоит → стоячая поза (ROM 1bb82 bcs)
    a.walkAcc += spd;
    int n = nf - 1; if (n > 8) n = 8;                           // ходячих суб-кадров (LUT покрывает 1..8)
    return WALK_PAT[n - 1][(a.walkAcc >> 3) & 31];              // значения 1..n
}

// ВЫСТРЕЛ В ТРУП (ZT corpse-hHit 0x18832 и клоны): отлёт ОТ игрока по ОРУЖИЮ + $35++ → у трупов с кадром-по-$35
// (Sgt/Imp/FH-SF/FH) спрайт меняется на «простреленный». Труп снова заскользит (timer).
// ⭐raw = 0x400−X = дист-кривая ОРУЖИЯ (playerWeaponRawDamage). ROM: v = unit·(0x400−X)/16 world-units → /256 клетки
// (вдвое слабее живого /8). Дальше/слабее оружие → меньше отлёт (было: КОНСТАНТА 0.18 — потеряна зависимость).
// ROM corpse-hHit НЕ спавнит искру (только отлёт + $35++) — spawnSparkA убрана.
inline void corpseHit(Actor& a, double fromX, double fromY, int raw) {
    double dx = a.x - fromX, dy = a.y - fromY, d = std::hypot(dx, dy);
    double k = (raw > 0 ? raw : 0) / 16.0 / 256.0;                      // (0x400−X)/16 units → клетки
    if (d > 0.01) { a.vx = dx / d * k; a.vy = dy / d * k; } else { a.vx = a.vy = 0; }
    a.timer = 8; ++a.state;                                             // снова скользит + фаза $35 (смена кадра «простреленный»)
}

// ── HIT-SCAN УРОН: луч от (px,py) вдоль (dx,dy) — если встречает врага раньше стены, наносит урон. ──
// Возвращает true, если попал во врага (тогда искру ставим во враге), и точку попадания (hx,hy).
inline bool damageRay(const Level& lvl, int floor, double px, double py, double dx, double dy,
                      int weaponId, double& hx, double& hy) {
    double x = px, y = py;
    for (int i = 0; i < 1024; ++i) {
        // враг в текущей клетке луча? (живой — приоритет)
        for (auto& a : actors()) {
            if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
            if (std::fabs(a.x - x) < 0.5 && std::fabs(a.y - y) < 0.5) {
                hx = a.x; hy = a.y;
                double dd = gameDist(a.x - px, a.y - py);              // дистанция (октаг. d7c0) → дист-урон оружия
                int raw = playerWeaponRawDamage(weaponId, dd);         // ROM-урон (для гейта крови ≥0x338)
                int dmg = raw > 0 ? (raw / 100 < 1 ? 1 : raw / 100) : 0;
                hitEnemy(a, dmg, px, py, raw);                         // урон + отлёт + кровь(по raw) + труп/искра
                return true;
            }
        }
        // труп в клетке луча (нет живого врага тут) → простреливание (смена кадра), луч останавливается
        for (auto& a : actors()) {
            if (!a.active || a.think != AT_CORPSE || a.floor != floor) continue;
            if (std::fabs(a.x - x) < 0.5 && std::fabs(a.y - y) < 0.5) {
                corpseHit(a, px, py, playerWeaponRawDamage(weaponId, gameDist(a.x - px, a.y - py))); hx = a.x; hy = a.y; return true;
            }
        }
        double nx = x + dx * 0.06, ny = y + dy * 0.06;
        int cx = (int)nx, cy = (int)ny;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) break;
        int ci = liveAlarmCamAt(floor, cx, cy);                                   // КАМЕРА-ТРЕВОГА в клетке луча → СБИТЬ (1 хит), тревога не сработает
        if (ci >= 0) { destroyAlarmCam(ci); hx = cx + 0.5; hy = cy + 0.5; return true; }
        if (cellBlockedAt(lvl.cellType(floor, cx, cy), nx - cx, ny - cy)) {        // ДИАГОНАЛЬ учитывается (полуплоскость)
            if (wallIsSecret(lvl.cellType(floor, cx, cy))) requestDestruct(floor, cx, cy);  // ТОЛЬКО секрет-стены (a6bc: смена текстуры); 0x83/84 пуля НЕ ломает (тревога-камера)
            break;
        }
        x = nx; y = ny;
    }
    hx = x; hy = y;
    return false;
}

// ⭐ ВЫСТРЕЛ ПО КАМЕРЕ-ТРЕВОГЕ (celltype 0x26): камера смонтирована ВЫСОКО (спрайт у потолка), её флаги +0x4 = 0x84
// (бит 0x80). ROM cone-hitscan 167e8 поражает актёра ⟺ `+0x4 & стойка-маска ≠ 0`. Стойка-маски: прыжок 0x80 / стойка
// 0x40 / присед 0x08 (167ce по питчу). 0x84 & 0x80 = 0x80, & 0x40 = 0, & 0x08 = 0 → камеру можно сбить ТОЛЬКО В ПРЫЖКЕ
// (целясь вверх). Трасса луча по клеткам до 1-й стены; живая камера в клетке до стены → destroyAlarmCam (1 хит → взрыв,
// тревога НЕ срабатывает, ZT hHit 15e4e). Гейт по стойке — у вызывающего (fireSpawn проверяет playerStanceMask()==0x80).
inline bool traceCameraHit(const Level& lvl, int floor, double px, double py, double dx, double dy) {
    double x = px, y = py;
    for (int i = 0; i < 600; ++i) {                              // до 1-й стены (как traceMiss, ~31 кл)
        double nx = x + dx * 0.06, ny = y + dy * 0.06; int cx = (int)nx, cy = (int)ny;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return false;
        int ci = liveAlarmCamAt(floor, cx, cy);                  // ЖИВАЯ камера в клетке луча → сбить
        if (ci >= 0) { destroyAlarmCam(ci); return true; }
        if (cellBlockedAt(lvl.cellType(floor, cx, cy), nx - cx, ny - cy)) return false;  // стена раньше камеры → мимо
        x = nx; y = ny;
    }
    return false;
}

// Урон всем врагам в радиусе r от точки (для огня/взрывов). Возвращает число задетых.
inline int damageEnemiesAt(int floor, double x, double y, double r, int dmg) {
    int n = 0;
    for (auto& a : actors()) {
        if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
        if (std::hypot(a.x - x, a.y - y) <= r) { ++n; hitEnemy(a, dmg, x, y); }   // урон + отлёт от взрыва/огня + труп
    }
    return n;
}

// Взрыв гранаты/ракеты/мины: спавн объекта-взрыва. ⭐ROM-ТАЙМИНГ (13dcc→14196): урон-рассылка (blast 16294)
// происходит НЕ сразу, а на 2-м ТИКЕ жизни взрыва ($1e==2) → цепная детонация распространяется с задержкой
// +2 тика на звено (как в игре). state=1 = «blast ещё не сработал» (AT_EXPLOSION think зовёт blastAt).
inline void explodeAt(const Level& lvl, double x, double y, int f, const Camera& cam, double z = 0.5) {  // z = высота очага (0 пол / 0.5 глаз)
    (void)lvl; (void)cam;                                        // урон — отложенно в blastAt (тик 2)
    Actor& e = spawnExplosionA(x, y, f, z);
    e.state = 1;                                                 // pending blast
    snd::ev(snd::SFX_EXPLOSION);                                 // звук взрыва (13dcc: 0x8A сразу при спавне)
}
// ДЕТОНАЦИЯ АКТЁРА-ВЗРЫВЧАТКИ (ROM receive 13ad2 → 13dcc: актёр «становится взрывом»): снять + спавн pending-взрыва.
inline void detonateActor(const Level& lvl, Actor& a, const Camera& cam) {
    double z = (a.think == AT_MINE) ? 0.0 : a.z;
    a.active = false;
    explodeAt(lvl, a.x, a.y, a.floor, cam, z);
}
// BLAST-рассылка (ZT 16294, зовётся think'ом взрыва на 2-м тике): радиус 4 кл, урон = ДИСТАНЦИЯ-FALLOFF
// (0x400−dist врагу, 16−dist·4 игроку) → у центра убивает, на 4 кл — 0. Нокбэк = через hitEnemy (стаггер-отлёт).
inline void blastAt(const Level& lvl, double x, double y, int f, const Camera& cam) {
    // ЗT 9e1e: взрыв «ПРОЯВЛЯЕТ» латентных врагов у очага (спавн pending-маркеров в радиусе, БЕЗ LOS-гейта) —
    // чтобы дремлющий враг, ещё не видевший игрока и потому не инстанцированный, попал под урон ниже.
    { auto& p = pendingSpawns();
      for (size_t i = 0; i < p.size();) {
          if (p[i].floor == f && std::abs(p[i].x + 0.5 - x) < 4.0 && std::abs(p[i].y + 0.5 - y) < 4.0 &&
              spawnOneEnemy(lvl, f, p[i].x, p[i].y, p[i].ct)) {   // материализуем (state0 — НЕ авто-агро); провал alloc → маркер остаётся
              p[i] = p.back(); p.pop_back();
          } else ++i;
      } }
    for (auto& a : actors()) {                                   // ВРАГИ в радиусе 4 кл С LOS: урон (0x400−dist)/100 + отлёт (ZT 16294)
        if (!a.active || a.think != AT_ENEMY || a.floor != f) continue;
        double dd = gameDist(a.x - x, a.y - y);                 // октаг. дистанция от центра взрыва
        // LOS-гейт (ZT 166e8): взрыв НЕ бьёт сквозь стены. Фильтра по AI-состоянию НЕТ → дремлющий враг тоже гибнет.
        if (dd < 4.0 && enemyLOS(lvl, f, x, y, a.x, a.y)) {
            int raw = (int)(1024.0 - dd * 256.0); int dmg = raw / 100; if (dmg < 1) dmg = 1; hitEnemy(a, dmg, x, y, raw);
        }
        // ⚠ НЕТ wake-агро на взрыв: в ZT (16294) звукового агро/алерта не существует (проверено дизасмом). Враги
        //   агрятся только по дистанции+LOS штатным AI; «проявление» латентных = грид-reveal выше, не бег на шум.
    }
    // ИГРОК: zdamage(dist)=16−dist·4; ⭐LOS-гейт (ZT 16294→165d8): сквозь стену игрока взрыв НЕ задевает.
    if (f == cam.floor && enemyLOS(lvl, f, x, y, cam.px, cam.py)) {
        double dd = gameDist(cam.px - x, cam.py - y);
        if (playerFighter() == 1) dd *= 2.0;                    // ⭐БОЕЦ 1 (демо): d0<<=1 → полурадиус/полуурон (162xx)
        int dmg = zdamage(dd); if (dmg > 0) damagePlayer(dmg, cam.px, cam.py, x, y); }
    // ЦЕПНАЯ ДЕТОНАЦИЯ (ZT 16294 шлёт receive ВСЕМ актёрам; взрывчатка 13ad2: dist<0x200=2кл → 13dcc).
    // Каждое звено взрывается со своей задержкой +2 тика (pending blast) — цепь «бежит», как в игре.
    for (auto& a : actors()) {
        if (!a.active || a.floor != f) continue;
        // ТОЛЬКО игроковы взрывчатые: мины, ракеты (AT_BULLET всегда игрок), гранаты игрока (state==0).
        // ВРАЖЕСКИЕ снаряды/гранаты (AT_ENEMY_SHOT, AT_GRENADE state==1) НЕ детонируем — они «на» врагах → спрайт взрыва ложился на врага.
        bool chainable = (a.think == AT_MINE) || (a.think == AT_BULLET) || (a.think == AT_GRENADE && a.state == 0);
        if (!chainable) continue;
        if (a.x == x && a.y == y) continue;                     // не сам очаг
        if (gameDist(a.x - x, a.y - y) >= 2.0) continue;        // ZT 0x200 = 2 кл
        detonateActor(lvl, a, cam);                             // → pending-взрыв (+2 тика)
    }
    { auto& vc = alarmCams();                                   // ВЗРЫВ валит камеры-тревоги в радиусе (ZT 16294 → receive/hHit 15e4e = 1 хит)
      for (size_t i = 0; i < vc.size(); ++i)
          if (!vc[i].dead && vc[i].floor == f && gameDist(vc[i].x + 0.5 - x, vc[i].y + 0.5 - y) < 4.0
              && enemyLOS(lvl, f, x, y, vc[i].x + 0.5, vc[i].y + 0.5)) destroyAlarmCam((int)i);
    }
    // ⚠ BLAST НЕ РАЗРУШАЕТ стены/двери (ZT 16294 деструкцию не зовёт). Секрет-стены ломает ФИТИЛЬ гранаты
    //   (13bf8→a6bc 3×3) и импакт ракеты (14114→a6bc) — см. AT_GRENADE/AT_BULLET в actors.cpp.
}

// АНТИ-ОВЕРЛАП враг↔игрок (ZT 0x146fe: при дист<0x20 толкает актёра наружу к player+unit·0x20). В порте враг,
// доезжая до точной позиции игрока, оказывался на дист~0 → его билборд кулился (f<0.05) → «невидим и движется ровно
// со мной». Держим МИНИМУМ FLOOR клетки от игрока (>ZT 0x20=0.125, увеличено для читаемости спрайта на близи).
inline void enemyPlayerStandoff(Actor& a, const Camera& cam, const Level& lvl) {
    if (a.floor != cam.floor) return;
    // ⭐0.125 = ZT 0x20 (2026-07-22): было 0.34 «для читаемости спрайта» — из-за этого мили-удары
    // (Imp 0x64=0.39 / Revenant-контакт 0x40=0.25) еле доставали порог: враг висел на границе.
    // В ROM враг подходит до 0x20 → контакт стабильный → набегание на ближников опасно, как в оригинале.
    const double FLOOR = 0.125;
    double pdx = a.x - cam.px, pdy = a.y - cam.py, pd = std::hypot(pdx, pdy);
    if (pd >= FLOOR) return;
    double tx, ty;
    if (pd > 1e-3) { tx = cam.px + pdx / pd * FLOOR; ty = cam.py + pdy / pd * FLOOR; }
    else           { tx = cam.px + FLOOR; ty = cam.py; }    // точно на игроке → вытолкнуть вперёд
    // ⭐выталкивание С ПРОВЕРКОЙ СТЕН: раньше игрок, прижав врага к стене/двери, ВЫДАВЛИВАЛ его внутрь
    // solid-клетки («пролез сквозь стену и застрял»). Блокировано → пробуем по осям → иначе не двигаем.
    auto blocked = [&](double x, double y) {
        int cx = (int)x, cy = (int)y;
        return enemyBlockedAt(lvl.cellType(a.floor, cx, cy), a.floor, cx, cy, x - cx, y - cy, false);
    };
    if      (!blocked(tx, ty))  { a.x = tx; a.y = ty; }
    else if (!blocked(tx, a.y)) { a.x = tx; }
    else if (!blocked(a.x, ty)) { a.y = ty; }               // иначе стоим: лучше оверлап, чем в стене
}

// ── THINK + рендер всех актёров (раз в кадр). cam — игрок (для AI). Наполняет worldFx (очередь спрайтов). ──
void updateActors(const Level& lvl, const Camera& cam);   // тело в actors.cpp
