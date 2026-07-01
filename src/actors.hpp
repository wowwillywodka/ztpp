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
    uint8_t tile = 0;          // тайл объект-банка   +0x3e
    uint8_t srcType = 0;       // исходный celltype врага
    int     drop = -1;         // ТРУП: оброненное оружие (id 1..14), подбирается при шаге на труп; −1 = нет
    bool    burned = false;    // обгорел в огне → труп «сожжённый» (тёмный/обугленный)
};

// ── ПУЛ (ZT: 64 слота). ФИКСИРОВАННЫЙ размер (без push_back!) — спавн во время think не должен
// реаллоцировать вектор и инвалидировать итераторы (взрыв гранаты спавнит актёров прямо в цикле). ──
static const int MAX_ACTORS = 256;
inline std::vector<Actor>& actors() { static std::vector<Actor> v(MAX_ACTORS); return v; }
inline void clearActors() { for (auto& a : actors()) a.active = false; worldFx().clear(); }
inline Actor& allocActor() {
    for (auto& a : actors()) if (!a.active) { a = Actor{}; a.active = true; return a; }
    return actors()[0];                      // пул полон — fallback (перезапишем слот 0)
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
    int    iframe = 0;               // кадры неуязвимости после урона (ZT -0x721e: tst→bne rts)
    int    flash = 0;                // ВСПЫШКА УРОНА (палитра краснеет/белеет, d98e) — сила по урону, затухает
    int    flashPeak = 1;            // пик flash (для фазы белый→красный)
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
};
inline PlayerState& player() { static PlayerState p; return p; }
inline void resetPlayerHP() { PlayerState& p = player(); p.hp = p.maxHp; p.iframe = 0; p.flash = 0; p.dead = false;
    p.knockVx = p.knockVy = 0; p.knockTimer = 0; p.knockPitch = 0; p.jumpVel = p.jumpY = 0; p.crouchY = 0; }
// Урон игроку (как d800): i-frame + броня + вспышка палитры + ОТСКОК от источника (sx,sy) + смерть.
// godmode = ЗАМОРОЗКА HP: удар ОБРАБАТЫВАЕТСЯ (вспышка/отскок/i-frame), но HP не уменьшается.
inline void damagePlayer(int dmg, double px, double py, double sx, double sy) {
    PlayerState& p = player();
    if (p.iframe > 0 || p.hp <= 0 || dmg <= 0) return;
    // БРОНЯ поглощает удар ЦЕЛИКОМ (HP не падает), −10% за попадание; кончилась → урон идёт в HP (юзер verif.).
    bool absorbed = false;
    if (!p.godmode && p.armor > 0) { p.armor -= 10; if (p.armor < 0) p.armor = 0; absorbed = true; }
    if (!p.godmode && !absorbed) { p.hp -= dmg; if (p.hp < 0) p.hp = 0; }   // god: HP заморожен; броня: HP не трогаем
    p.iframe = 20;
    p.flash = 6 + (dmg < 9 ? dmg : 9); if (p.flash > 15) p.flash = 15; p.flashPeak = p.flash;  // сила вспышки ~ урон
    double dx = px - sx, dy = py - sy, d = std::hypot(dx, dy);   // отскок ОТ источника
    if (d > 0.01) { double k = 0.06 + 0.004 * dmg; if (k > 0.16) k = 0.16; p.knockVx = dx / d * k; p.knockVy = dy / d * k; }
    if (dmg >= 6) p.knockTimer = 20;                             // СИЛЬНЫЙ удар (взрыв/босс) → НОКДАУН (ZT word0≥7: -0x71dc=0x14)
    if (p.hp == 0) p.dead = true;
}
inline void healPlayer(int amt) { PlayerState& p = player(); p.hp += amt; if (p.hp > p.maxHp) p.hp = p.maxHp; }

// ── ЭФФЕКТ-параметры ──
static const int A_EXPL_FRAMES  = 8;
static const int A_SPARK_FRAMES = 6;     // искра стрелкового: 3 кадра анимации 63→64→65 (по 2 тика)
static const int A_FIRE_LIFE    = 20;    // огонь огнемёта горит ~20 кадров и тухнет (lifetime $34, дизасм 0x142c6)
static const uint8_t A_EXPL_TILE  = 1;   // огненный шар (взрыв ракеты/гранаты)
static const uint8_t A_SPARK_SEQ[3] = {63, 64, 65};  // искра пули: вспышка→кольцо→рассеивание (объект-банк)
static const uint8_t A_FIRE_TILE  = 0;   // пламя/огонь (объект-банк 0)

// ── СПАВН эффектов ──
inline void spawnBullet(double x, double y, int f, double dx, double dy, double sp, uint8_t tile, int life) {
    Actor& a = allocActor(); a.think = AT_BULLET; a.x = x; a.y = y; a.vx = dx * sp; a.vy = dy * sp;
    a.floor = f; a.tile = tile; a.timer = life;
}
// Огнемёт: частица пламени летит вперёд, жжёт врагов, ГАСНЕТ за lifetime или у стены (стены НЕ поджигаются).
inline void spawnFlameP(double x, double y, int f, double dx, double dy) {
    Actor& a = allocActor(); a.think = AT_FLAME; a.x = x; a.y = y; a.vx = dx * 0.26; a.vy = dy * 0.26;
    a.floor = f; a.tile = A_FIRE_TILE; a.timer = 12;         // ~12 кадров горит на лету и тухнет
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
    a.vx = dx * 0.12; a.vy = dy * 0.12;       // лёгкий нос вперёд
    a.z = 0.45; a.vz = -0.05;                 // из дула на уровне рук → ПАДАЕТ вниз (гравитация в think)
    a.timer = 16;                             // ZT: $1e 0..0x10, спрайт сжимается с возрастом
}
inline void spawnMine(double x, double y, int f) {
    Actor& a = allocActor(); a.think = AT_MINE; a.x = x; a.y = y; a.floor = f; a.tile = 8;  // мина (объект-банк 8)
    a.timer = 21;   // АРМИНГ (ZT 0x12bfe: ставит actor+$1f=0x14 → первый think через ~21 кадр) — НЕ детонирует сразу на ставившем
}
// Граната (think 0x13adc): бросается с дугой (z вверх + гравитация), горизонт. скорость, фитиль $34=50.
inline void spawnGrenade(double x, double y, int f, double dx, double dy, int owner = 0) {
    Actor& a = allocActor(); a.think = AT_GRENADE; a.x = x; a.y = y; a.floor = f;
    a.vx = dx * 0.12; a.vy = dy * 0.12;     // горизонт. бросок
    a.z = 0.35; a.vz = 0.028;               // из руки, невысокая дуга
    a.timer = 50; a.tile = 16;              // фитиль 50 тиков ($34); ЛЕТЯЩАЯ граната = декор-тайл 16 (ZT draw 0x13f3e bank 0x1109be); пикап-граната=13, проектиль=16!
    a.state = owner;                        // 0=граната игрока (детонир. у врага), 1=граната врага (детонир. у игрока)
}
inline void spawnSparkA(double x, double y, int f) {
    Actor& a = allocActor(); a.think = AT_SPARK; a.x = x; a.y = y; a.floor = f; a.tile = A_SPARK_SEQ[0];
}
inline void spawnExplosionA(double x, double y, int f, double z = 0.5) {  // z = визуальная высота (0 пол, 0.5 ур.глаз)
    Actor& a = allocActor(); a.think = AT_EXPLOSION; a.x = x; a.y = y; a.floor = f; a.tile = A_EXPL_TILE; a.z = z;
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
inline void spawnBlood(double x, double y, int f, double dirX, double dirY, int count) {
    if (!faBlood()) return;
    int live = 0;                                              // кап: ≤10 частиц одновременно (в оригинале крови НЕМНОГО)
    for (auto& a : actors()) if (a.active && a.think == AT_BLOOD) ++live;
    double dl = std::hypot(dirX, dirY); if (dl > 0.01) { dirX /= dl; dirY /= dl; } else { dirX = dirY = 0; }
    for (int i = 0; i < count && live < 10; ++i, ++live) {
        Actor& a = allocActor(); a.think = AT_BLOOD; a.x = x; a.y = y; a.floor = f; a.state = 0; a.timer = 0; a.frameT = 0;
        // ZT 0x157ca: vel = base d3/d4 + rnd((&0x7f)−0x40) = ±0x40/256 = ±0.25 кл/кадр (разброс ШИРОКИЙ, доминирует);
        // z-off $24 = rnd((&0xf)−8) = −8..7 → /64; z-vel $2e = rnd((&0xf)−9) = −9..6 → /64 (лёгкий разлёт, гравитация тянет).
        a.vx = dirX * 0.06 + bloodRnd() * 0.25;               // широкий конус по направлению удара
        a.vy = dirY * 0.06 + bloodRnd() * 0.25;
        a.z  = 0.5 + bloodRnd() * 0.12;                        // около центра тела ($24 −8..7 /64)
        a.vz = bloodRnd() * 0.12 - 0.02;                      // $2e −9..6 /64: чаще вверх, слегка вниз-смещён
        a.tile = A_BLOOD_FLY;
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
inline void spawnCorpse(double x, double y, int f, uint8_t ct, double kvx = 0, double kvy = 0, uint8_t variant = 0, int drop = -1, bool burned = false) {
    Actor& a = allocActor(); a.think = AT_CORPSE; a.x = x; a.y = y; a.floor = f; a.srcType = ct; a.variant = variant;
    a.vx = kvx; a.vy = kvy; a.timer = 14;   // труп отлетает от смертельного удара и скользит ~14 кадров (трейс: 0.25-0.4 кл/кадр, затух.)
    a.drop = drop; a.burned = burned;       // сожжён огнём → обугленный труп
    snd::ev(snd::SFX_ENEMY_DEATH);          // ЗВУК смерти врага (ZT 0x2d, FH death 0x184e6)
}
// Подбор оброненного оружия при шаге на труп: ищет труп под игроком с drop≥0, возвращает id (и снимает drop). cb добавляет в инвентарь.
template <typename AddFn>
inline void corpsePickup(const Camera& cam, AddFn add) {
    for (auto& a : actors()) {
        if (!a.active || a.think != AT_CORPSE || a.drop < 0 || a.floor != cam.floor) continue;
        if (std::hypot(cam.px - a.x, cam.py - a.y) < 0.6) { if (add(a.drop)) a.drop = -1; }  // взял → снять
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
// УРОН ОРУЖИЯ ИГРОКА врагу (ZT хендлеры 0x12eac/0x1303c/…): враг получает (0x400 − X)/100, где X — дист-функция оружия:
//   handgun/laser X=dist²/512 (квадрат. спад, ~2.8кл), shotgun X=dist (лин., 4кл), pulse X=dist/2 (дальнобой, 8кл),
//   огнетушитель X=0x3e8 (фикс, почти 0). HP врага = ROM/100. Любое попадание в дальности → ≥1.
inline int playerWeaponDamage(int id, double distCells) {
    double du = distCells * 256.0, X;
    switch (id) {
        case 8: case 10: X = du * du / 512.0; break;   // HANDGUN/LASER — квадратичный
        case 12:         X = du;             break;     // SHOTGUN — линейный
        case 14:         X = du * 0.5;       break;     // PULSE LASER — дальнобойный (½)
        case 4:          if (distCells > 1.17) return 0; X = 1000.0; break;  // FIRE EXT — фикс, дальность <0x12c

        default:         X = du;             break;     // прочий hitscan ≈ shotgun
    }
    if (X >= 1024.0) return 0;                           // вне дальности (X ≥ 0x400)
    int dmg = (int)((1024.0 - X) / 100.0);
    return dmg < 1 ? 1 : dmg;
}
// Параметры движения врага. ВЕРИФ. ПО ДИЗАСМУ: think вызывается КАЖДЫЙ кадр (планировщик 0x1327a: subq.b #1,$1f;
// bpl skip; think очищает $1f=0 → −1<0 каждый кадр), а move 0x145aa применяет ПОЛНУЮ скорость каждый think →
// per-frame: gain=1/2^shift, vmax=clampUnits/256 (БЕЗ деления на 7 — это была ошибка, враги были ×7 медленнее).
// РЕГУЛИРУЕМЫЙ множитель скорости врагов (CLOCK-зависимая величина — единственная, что требует подстройки под
// фактический темп боя; 1.0 = дизасм-точно при 60Гц). Меняется в рантайме (хоткеи [ / ] в main, сохраняется в ini).
inline double& enemySpeedScale() { static double v = 0.8; return v; }
struct EMove { double gain, vmax; };
inline EMove enemyMove(uint8_t ct) {
    double E = enemySpeedScale();
    switch (enemyClass(ct)) {
        case EC_RANGED_FAST: return {1.0/2*2, 0x40/256.0*E};  // Revenant: >>1 ×2, ±0x40 (быстрейший дальний)
        case EC_GRENADIER:   return {1.0/8,   0x2d/256.0*E};  // FH-SF: >>3, ±0x2d
        case EC_MELEE:       return {1.0/4,   0x1e/256.0*E};  // Imp: >>2, ±0x1e
        case EC_HOPPER:      return {1.0/8,   0x28/256.0*E};  // Hydaca chase: >>3, ±0x28
        case EC_BOSS_PROJ:   return {1.0/8,   0x0c/256.0*E};  // Boss1: >>3, ±0x0c (медленный)
        case EC_BOSS_MELEE:  return {1.0/4,   0x20/256.0*E};  // Boss3: >>2, ±0x20 (быстрый чардж)
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
// PRNG для ИИ врага (как ZT jsr $a5c — случайный сдвиг точки прицела ±1 клетка). LCG, детерминир.
inline uint32_t enemyRng() { static uint32_t s = 0x13579bdfu; s = s * 1664525u + 1013904223u; return s; }
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
        int cx = (int)(ex + dx * t), cy = (int)(ey + dy * t);
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) return false;
        uint8_t ct = lvl.cellType(floor, cx, cy);
        if (cellBlocks(ct)) return false;                         // стена закрывает обзор
        if (cellIsDoor(ct) && doorOpen(floor, cx, cy) < 0.4) return false;  // ЗАКРЫТАЯ дверь блокирует; ОТКРЫТАЯ — пропускает
    }
    return true;
}

// Видимый снаряд врага. mode: 0=прямой, 1=САМОНАВОД (Boss1 0x15f6e — довод курса к игроку), 2=ГРАНАТА (FH-SF 0x13adc —
// летит по ДУГЕ: z вверх+гравитация, медленнее, крупнее). a.state=mode. Урон игроку по близости; у стены/двери — искра.
inline void spawnEnemyShot(double x, double y, int f, double dx, double dy, int mode = 0) {
    Actor& a = allocActor(); a.think = AT_ENEMY_SHOT; a.x = x; a.y = y; a.floor = f;
    a.state = mode;
    if (mode == 2) { a.vx = dx * 0.10; a.vy = dy * 0.10; a.z = 0.12; a.vz = 0.045;  // граната: дуга, медленнее
                     a.tile = A_EXPL_TILE; a.timer = 110; }
    else           { a.vx = dx * 0.16; a.vy = dy * 0.16; a.tile = A_SPARK_SEQ[0]; a.timer = 80; }
}

// Создать ОДНОГО врага в клетке (x,y) этажа.
inline void spawnOneEnemy(const Level& lvl, int floor, int x, int y) {
    uint8_t ct = lvl.cellType(floor, x, y);
    Actor& a = allocActor(); a.think = AT_ENEMY; a.floor = floor;
    a.x = x + 0.5; a.y = y + 0.5; a.hp = enemyHp(ct); a.srcType = ct; a.tile = enemyTileForCt(ct);
    a.homeX = a.x; a.homeY = a.y;                                          // дом = точка спавна
    a.state = 0; a.timer = 0; a.fireCd = 20 + ((x * 7 + y * 13) % 40);
    // Sgt (0x29): таймер трансформации в FH-SF (ZT state6 0x1b51a — морф человека в инопланетянина по истечении времени).
    a.xformT = (ct == 0x29) ? (240 + ((x * 11 + y * 17) % 180)) : 0;       // ~4-7с до морфа (дизасм +0x41=50-81 тиков)
    a.variant = (ct == 0x2A) ? (uint8_t)((x * 5 + y * 3) & 1) : 0;         // только FH: 2 визуальные вариации (Hydaca драйвит variant по z)
    if (ct == 0x65) { a.z = 1.0; a.variant = 1; }                          // Hydaca стартует НА ПОТОЛКЕ (висит, потом падает)
}
// Спавн врага ЗАДАННОГО типа в точке (для консоли). ct — celltype врага (0x29..0x6b).
inline void spawnEnemyByType(int floor, double x, double y, uint8_t ct) {
    Actor& a = allocActor(); a.think = AT_ENEMY; a.floor = floor;
    a.x = x; a.y = y; a.hp = enemyHp(ct); a.srcType = ct; a.tile = enemyTileForCt(ct);
    a.homeX = x; a.homeY = y; a.state = 0; a.timer = 0; a.fireCd = 20;
    a.xformT = (ct == 0x29) ? 300 : 0;
    if (ct == 0x65) { a.z = 1.0; a.variant = 1; }
}
// ПРОКСИ-СПАВН (ZT 0x15d18: скан 11×11 ±5 кл вокруг игрока — враги ПОЯВЛЯЮТСЯ когда подходишь, не все при загрузке!).
// Маркеры этажа собираются в pending; updateEnemySpawns каждый кадр спавнит те, что в радиусе. Это убирает «враги
// активны/несутся со старта по всей карте» — они материализуются по мере продвижения, как в оригинале.
struct PendingSpawn { int x, y; };
inline std::vector<PendingSpawn>& pendingSpawns() { static std::vector<PendingSpawn> v; return v; }
inline void collectEnemyMarkers(const Level& lvl, int floor) {        // при загрузке/смене этажа: собрать маркеры (НЕ спавнить)
    auto& p = pendingSpawns(); p.clear();
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (cellIcon(lvl.cellType(floor, x, y)) == 9) p.push_back({x, y});
}
// КАРТА-ОГОНЬ: клетки celltype 0x18 (Flame) → вечный AT_FIRE (хазард). Вызывать при загрузке/смене этажа.
inline std::unordered_set<int>& fireExtinguished() { static std::unordered_set<int> s; return s; }  // потушенные клетки (floor*1024+y*32+x)
inline void spawnMapFires(const Level& lvl, int floor) {
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (lvl.cellType(floor, x, y) == 0x18) {
                if (fireExtinguished().count(floor * 1024 + y * 32 + x)) continue;   // потушено игроком — не возрождать
                spawnFire(x + 0.5, y + 0.5, floor, -1);                              // вечный огонь-хазард
            }
}
inline void updateEnemySpawns(const Level& lvl, int floor, double px, double py, double range = 5.5) {
    auto& p = pendingSpawns();
    for (size_t i = 0; i < p.size(); ) {                              // спавн маркеров в радиусе (box ±range) от игрока
        double mx = p[i].x + 0.5, my = p[i].y + 0.5;
        // LOS-гейт: не спавнить СКВОЗЬ стену/ЗАКРЫТУЮ дверь (юзер: «триггерятся до того как дверь открыл»)
        if (std::abs(px - mx) <= range && std::abs(py - my) <= range && enemyLOS(lvl, floor, mx, my, px, py)) {
            spawnOneEnemy(lvl, floor, p[i].x, p[i].y); p[i] = p.back(); p.pop_back();
        } else ++i;
    }
}
// Враги ОТКРЫВАЮТ дверь, на/у которой стоят (чтобы проходить через двери, как игрок — а не клипать закрытую).
inline void openDoorsAtEnemies(const Level& lvl, int floor) {
    auto& m = doorMap();
    for (auto& a : actors()) {
        if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
        int cx = (int)a.x, cy = (int)a.y;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) continue;
        if (!cellIsDoor(lvl.cellType(floor, cx, cy))) continue;
        int k = doorKey(floor, cx, cy);
        double o = (m.count(k) ? m[k] : 0.0) + 0.20; if (o > 1) o = 1; m[k] = o;
    }
}
// Спавн ВСЕХ врагов сразу (для дампа/скриншота — не геймплей).
inline void spawnEnemiesFromLevel(const Level& lvl, int floor) {
    for (int y = 0; y < Level::H; ++y)
        for (int x = 0; x < Level::W; ++x)
            if (cellIcon(lvl.cellType(floor, x, y)) == 9) spawnOneEnemy(lvl, floor, x, y);
}

// УРОН ВРАГУ (ZT damage-handler 0x1B824): HP−=dmg; KNOCKBACK (отлёт ОТ источника, ∝ урону) + стаггер +
// искра-импакт; смерть HP≤0 → ТРУП (не взрыв). fromX/fromY = источник урона (для направления отлёта).
// ZT receive-handler (sub_0187c8): на КАЖДОМ попадании ставит state=2 (СТАГГЕР), обнуляет+задаёт нокбэк-вектор
// v=unit(от источника)·(урон>>3), вычитает HP (может уйти В МИНУС). СМЕРТЬ НЕ мгновенная — труп появляется в think
// ТОЛЬКО когда нокбэк-скорость ЗАТУХАЕТ ниже 0x0a И HP<0 (sub_01887a $188b2: tst $36; bmi death). Поэтому ПОКА
// СТРЕЛЯЕШЬ — каждый выстрел освежает скорость → враг ДЕРЖИТСЯ в стаггере; перестал → скорость села → труп.
inline void hitEnemy(Actor& a, int dmg, double fromX, double fromY) {
    a.hp -= dmg;
    double dx = a.x - fromX, dy = a.y - fromY, d = std::hypot(dx, dy);    // отлёт ОТ источника
    spawnBlood(a.x, a.y, a.floor, dx, dy, a.hp <= 0 ? 4 : 1);             // КРОВЬ: МАЛО (ZT: основной всплеск на убойный) — 1 капля/хит, 4 на смерть
    if (a.hp <= 0 && a.think == AT_ENEMY && a.srcType == 0x6A && !a.revived) {  // BOSS3: воскресает ОДИН раз (ZT 0x1a40c)
        a.revived = true; a.hp = enemyHp(0x6A) / 8; a.hitT = 1;            // ROM HP 0x3e8=1000 = 1/8 от полных 8000 → слабее фаза
        spawnSparkA(a.x, a.y, a.floor); return;
    }
    // НОКБЭК ZT: v = (вектор от источника)·(урон>>3). Порт-урон = ROM/100 → v ≈ dmg·0.049 кл/кадр; затух. ÷2/кадр.
    double k = dmg * 0.049; if (k > 0.55) k = 0.55;
    if (d > 0.01) { a.vx = dx / d * k; a.vy = dy / d * k; } else { a.vx = a.vy = 0; }
    if (a.hitT <= 0) a.hitT = 1;                                          // ВХОД в стаггер (выход — по затуханию v, не таймеру)
    // (НЕТ импакт-искры по врагу: искра 0x12ad4 — только промах в СТЕНУ; по врагу = стаггер-аним/кровь)
}

// ── HIT-SCAN УРОН: луч от (px,py) вдоль (dx,dy) — если встречает врага раньше стены, наносит урон. ──
// Возвращает true, если попал во врага (тогда искру ставим во враге), и точку попадания (hx,hy).
inline bool damageRay(const Level& lvl, int floor, double px, double py, double dx, double dy,
                      int weaponId, double& hx, double& hy) {
    double x = px, y = py;
    for (int i = 0; i < 1024; ++i) {
        // враг в текущей клетке луча?
        for (auto& a : actors()) {
            if (!a.active || a.think != AT_ENEMY || a.floor != floor) continue;
            if (std::fabs(a.x - x) < 0.5 && std::fabs(a.y - y) < 0.5) {
                hx = a.x; hy = a.y;
                double dd = gameDist(a.x - px, a.y - py);              // дистанция (октаг. d7c0) → дист-урон оружия
                hitEnemy(a, playerWeaponDamage(weaponId, dd), px, py); // урон + отлёт + труп/искра
                return true;
            }
        }
        double nx = x + dx * 0.06, ny = y + dy * 0.06;
        int cx = (int)nx, cy = (int)ny;
        if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H) break;
        if (cellBlockedAt(lvl.cellType(floor, cx, cy), nx - cx, ny - cy)) {        // ДИАГОНАЛЬ учитывается (полуплоскость)
            if (wallIsDestructible(lvl.cellType(floor, cx, cy))) requestDestruct(floor, cx, cy);  // выстрел в разруш./секрет-стену
            break;
        }
        x = nx; y = ny;
    }
    hx = x; hy = y;
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

// Взрыв гранаты/ракеты/мины (ZT radius 0x16294): радиус 4 кл, урон = ДИСТАНЦИЯ-FALLOFF (0x400−dist врагу, 16−dist·4
// игроку) → у центра убивает, на 4 кл — 0. Нокбэк = через hitEnemy (стаггер-отлёт). + area-разрушение стен.
inline void explodeAt(double x, double y, int f, const Camera& cam, double z = 0.5) {  // z = высота очага (0 пол / 0.5 глаз)
    spawnExplosionA(x, y, f, z);
    snd::ev(snd::SFX_EXPLOSION);                                 // звук взрыва
    for (auto& a : actors()) {                                   // ВРАГИ в радиусе 4 кл: урон (0x400−dist)/100 + отлёт
        if (!a.active || a.think != AT_ENEMY || a.floor != f) continue;
        double dd = gameDist(a.x - x, a.y - y);                 // октаг. дистанция от центра взрыва
        if (dd < 4.0) { int dmg = (int)((1024.0 - dd * 256.0) / 100.0); if (dmg < 1) dmg = 1; hitEnemy(a, dmg, x, y); }
        // ВЗРЫВ = ШУМ: будит врагов в радиусе слышимости (>урон-радиуса) → агро к игроку (ZT: урон→стаггер→AI, +алерт-союзников).
        // Дремлющие (state 0) триггерятся и СБЕГАЮТСЯ на взрыв/игрока.
        if (dd < 7.0 && a.state == 0) { a.state = 1; a.aimX = cam.px; a.aimY = cam.py; if (a.timer < 20) a.timer = 30; }
    }
    if (f == cam.floor) { double dd = gameDist(cam.px - x, cam.py - y);     // ИГРОК: zdamage(dist) = 16−dist·4 (до 16 в упор)
        int dmg = zdamage(dd); if (dmg > 0) damagePlayer(dmg, cam.px, cam.py, x, y); }
    // ЦЕПНАЯ ДЕТОНАЦИЯ (ZT 16294 шлёт receive-хендлер ВСЕМ актёрам; explodable детонируют — мина 0x13ad2: X<0x200=2кл):
    // взрыв подрывает мины/гранаты/ракеты/снаряды рядом. active=false ДО рекурсивного взрыва → без зацикливания.
    for (auto& a : actors()) {
        if (!a.active || a.floor != f) continue;
        // ТОЛЬКО игроковы взрывчатые: мины, ракеты (AT_BULLET всегда игрок), гранаты игрока (state==0).
        // ВРАЖЕСКИЕ снаряды/гранаты (AT_ENEMY_SHOT, AT_GRENADE state==1) НЕ детонируем — они «на» врагах → спрайт взрыва ложился на врага.
        bool chainable = (a.think == AT_MINE) || (a.think == AT_BULLET) || (a.think == AT_GRENADE && a.state == 0);
        if (!chainable) continue;
        if (a.x == x && a.y == y) continue;                     // не сам очаг
        if (gameDist(a.x - x, a.y - y) >= 2.0) continue;        // ZT 0x200 = 2 кл
        double ex = a.x, ey = a.y; int ef = a.floor; a.active = false;
        explodeAt(ex, ey, ef, cam);                             // вторичный взрыв (рекурсия безопасна: пул фикс., снят до вызова)
    }
    for (int dyc = -1; dyc <= 1; ++dyc)                          // ВЗРЫВ ломает разруш./секрет-стены вокруг (ZT скан 0x15c74)
        for (int dxc = -1; dxc <= 1; ++dxc) requestDestruct(f, (int)x + dxc, (int)y + dyc);
}

// АНТИ-ОВЕРЛАП враг↔игрок (ZT 0x146fe: при дист<0x20 толкает актёра наружу к player+unit·0x20). В порте враг,
// доезжая до точной позиции игрока, оказывался на дист~0 → его билборд кулился (f<0.05) → «невидим и движется ровно
// со мной». Держим МИНИМУМ FLOOR клетки от игрока (>ZT 0x20=0.125, увеличено для читаемости спрайта на близи).
inline void enemyPlayerStandoff(Actor& a, const Camera& cam) {
    if (a.floor != cam.floor) return;
    const double FLOOR = 0.34;
    double pdx = a.x - cam.px, pdy = a.y - cam.py, pd = std::hypot(pdx, pdy);
    if (pd >= FLOOR) return;
    if (pd > 1e-3) { a.x = cam.px + pdx / pd * FLOOR; a.y = cam.py + pdy / pd * FLOOR; }
    else { a.x = cam.px + FLOOR; a.y = cam.py; }            // точно на игроке → вытолкнуть вперёд
}

// ── THINK + рендер всех актёров (раз в кадр). cam — игрок (для AI). Наполняет worldFx (очередь спрайтов). ──
inline void updateActors(const Level& lvl, const Camera& cam) {
    auto& v = actors();
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
                    a.active = false; explodeAt(a.x, a.y, a.floor, cam);   // взрыв ракеты: урон врагам/игроку (снять ДО — анти-цепь-рекурсия)
                } else { a.x = nx; a.y = ny; if (--a.timer <= 0) { a.active = false; explodeAt(a.x, a.y, a.floor, cam); } }
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
                if (--a.timer <= 0 || nearTgt) { explodeAt(a.x, a.y, a.floor, cam, a.z); a.active = false; break; }  // взрыв на ВЫСОТЕ гранаты (z), не у глаз
                double nx = a.x + a.vx, ny = a.y + a.vy;   // горизонталь + отскок от стен (reflect+damp, как neg/asr)
                bool bounced = false;
                if (cellBlocks(lvl.cellType(a.floor, (int)nx, (int)a.y))) { a.vx = -a.vx * 0.5; nx = a.x; bounced = true; }
                if (cellBlocks(lvl.cellType(a.floor, (int)a.x, (int)ny))) { a.vy = -a.vy * 0.5; ny = a.y; bounced = true; }
                if (nx >= 0 && ny >= 0 && nx < Level::W && ny < Level::H) { a.x = nx; a.y = ny; }
                a.vz -= 0.0035; a.z += a.vz;               // гравитация ($2e -= ; $24 += )
                if (a.z <= 0) { a.z = 0; if (a.vz < -0.02) { a.vz = -a.vz * 0.4; a.vx *= 0.7; a.vy *= 0.7; bounced = true; } else a.vz = 0; }  // отскок от пола
                if (bounced && a.floor == cam.floor) snd::ev(snd::SFX_GRENADE_BOUNCE);   // ЗВУК отскока гранаты о стену/пол
                break;
            }
            case AT_FLAME: {                             // СТРУЯ огнемёта: летит, оставляет ОГОНЬ-ТРЕЙЛ вдоль пути, ГАСНЕТ у стены.
                // НЕ бьёт игрока (своё пламя) и не наносит прямой урон врагам — урон только от огня-трейла (AT_FIRE).
                if ((a.frameT++ & 1) == 0)                                            // каждый 2-й кадр оставляет огонь (трейл вдоль струи)
                    spawnFire(a.x, a.y, a.floor, 18, true);                           // player-safe: своё пламя не жжёт игрока
                double nx = a.x + a.vx, ny = a.y + a.vy;
                int cx = (int)nx, cy = (int)ny;
                if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                    cellBlocks(lvl.cellType(a.floor, cx, cy)) || --a.timer <= 0) a.active = false;  // тухнет у СТЕНЫ (не проходит) или по таймеру
                else { a.x = nx; a.y = ny; }
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
                if (a.state != 0) { if (++a.timer >= 48) a.active = false; break; }   // ПЯТНО стареет ~48 кадров и пропадает
                bool hitWall = false;
                for (int s = 0; s < 4; ++s) {            // 4 подшага с проверкой стены (ZT 16 подшагов, dbra d7)
                    double nx = a.x + a.vx * 0.25, ny = a.y + a.vy * 0.25;
                    int cx = (int)nx, cy = (int)ny;
                    if (cx < 0 || cy < 0 || cx >= Level::W || cy >= Level::H ||
                        cellBlocks(lvl.cellType(a.floor, cx, cy))) { hitWall = true; break; }
                    a.x = nx; a.y = ny;
                }
                a.z += a.vz; a.vz -= 0.03125;            // гравитация (ZT $24 += $2e; $2e -= 2 → /64)
                a.vx *= 0.75; a.vy *= 0.75;              // гориз. трение (ZT vel -= vel>>2 при отсутствии столкновения)
                if (hitWall)        { a.state = 1; a.vx = a.vy = a.vz = 0; a.tile = A_BLOOD_SPLAT; a.timer = 0; }       // пятно о СТЕНУ ($1a=0x16f02)
                else if (a.z <= 0)  { a.z = 0; a.state = 2; a.vx = a.vy = a.vz = 0; a.tile = A_BLOOD_SPLAT; a.timer = 0; } // пятно на ПОЛУ ($1a=0x16f2c)
                else if (++a.frameT > 40) a.active = false;   // страховка от «вечно висящей» капли
                break;
            }
            case AT_MINE: {                              // детонация: враг ИЛИ игрок ближе 1 кл → взрыв + урон
                if (a.timer > 0) { --a.timer; break; }   // АРМИНГ (ZT $1f=0x14): пока инертна — НЕ детонирует (защита ставившего)
                bool trig = (a.floor == cam.floor && gameDist(cam.px - a.x, cam.py - a.y) < 1.0);  // ZT 0x100 = 1 кл (игрок)
                if (!trig) for (auto& e : v) if (e.active && e.think == AT_ENEMY && e.floor == a.floor &&
                                                 gameDist(e.x - a.x, e.y - a.y) < 1.0) { trig = true; break; }  // или враг
                if (trig) { explodeAt(a.x, a.y, a.floor, cam, 0.0); a.active = false; }  // мина на ПОЛУ → взрыв низко
                break;
            }
            case AT_ENEMY: {                             // AI: ДИСПЕТЧ ПО КЛАССУ (по разбору capstone think-функций каждого типа)
                if (a.floor != cam.floor) break;
                // Sgt → FH-SF: трансформация человека в инопланетянина по истечении времени (ZT state6 0x1b51a).
                // Меняем srcType → класс остаётся EC_GRENADIER, а gfx-слот (Sgt→FH-SF) и спрайты сменяются автоматически.
                if (a.xformT > 0 && !enemiesFrozen()) {
                    if (--a.xformT == 0 && a.srcType == 0x29) {
                        a.srcType = 0x69; a.tile = enemyTileForCt(0x69);  // стал Former Human SF
                        a.state = 0; a.timer = 0; a.fireCd = 30;          // сброс боевого цикла после морфа
                    }
                }
                EClass ec = enemyClass(a.srcType);
                if (a.hitT > 0) {                                        // СТАГГЕР (ZT state 2, sub_01887a): выход по ЗАТУХАНИЮ скорости
                    double sp = std::hypot(a.vx, a.vy);
                    if (sp >= 0.039) {                                   // ZT 0x0a/256≈0.039 кл/кадр: ещё «летит» → скользим + ÷2, держимся в стаггере
                        double nx = a.x + a.vx, ny = a.y + a.vy;
                        if (!cellBlockedForEnemy(lvl.cellType(a.floor, (int)nx, (int)a.y), nx - (int)nx, a.y - (int)a.y)) a.x = nx;
                        if (!cellBlockedForEnemy(lvl.cellType(a.floor, (int)a.x, (int)ny), a.x - (int)a.x, ny - (int)ny)) a.y = ny;
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
                    double nx = a.x + a.vx, ny = a.y + a.vy;
                    double ex = nx + (a.vx > 0 ? R : -R);               // передний край по X
                    if (a.vx != 0 && !cellBlockedForEnemy(lvl.cellType(a.floor, (int)ex, (int)a.y), ex - (int)ex, a.y - (int)a.y)) a.x = nx;
                    double ey = ny + (a.vy > 0 ? R : -R);               // передний край по Y
                    if (a.vy != 0 && !cellBlockedForEnemy(lvl.cellType(a.floor, (int)a.x, (int)ey), a.x - (int)a.x, ey - (int)ey)) a.y = ny;
                    enemyPlayerStandoff(a, cam);                         // АНТИ-ОВЕРЛАП (ZT 0x146fe): не проваливаться в игрока
                    if (std::hypot(a.vx, a.vy) > 0.004) a.frameT += 2;   // кадр ходьбы (anim0) при движении
                };

                // ═══ DOG (0x19540): ДРЕМЛЕТ У ДОМА → погоня по ЖИВОМУ игроку при ОБНАРУЖЕНИИ → прыжок-укус ═══
                if (ec == EC_DOG) {
                    if (a.state == 0) {                                  // ПАТРУЛЬ У ДОМА (НЕ дрейф к игроку!): бродит ±1кл у спавна
                        if (los && d < 6.0) { a.state = 1; }             // ОБНАРУЖИЛ (LOS + близко) → погоня
                        else { moveTo(a.aimX, a.aimY, 0.22);
                            if (--a.timer <= 0) { a.aimX = a.homeX + ((int)(enemyRng() & 0x1ff) - 256) / 256.0;  // у ДОМА
                                                   a.aimY = a.homeY + ((int)(enemyRng() & 0x1ff) - 256) / 256.0; a.timer = 70; } }
                        break;
                    }
                    if (d > 11.0 && !los) { a.state = 0; a.timer = 1; break; }   // потерял → назад к дому
                    if (a.state == 1) { moveTo(cam.px, cam.py, 1.0);     // погоня — трек ЖИВОГО игрока, быстро
                        if (d < 1.2) { a.state = 2; a.timer = 14; a.aimX = cam.px; a.aimY = cam.py; } }
                    else { moveTo(a.aimX, a.aimY, 1.0);                  // прыжок к запомненной точке
                        if (d < 0.7 && a.fireCd <= 0) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireCd = 48; a.fireAnimT = 12; }
                        if (--a.timer <= 0) a.state = 1; }
                    break;
                }

                // ═══ HYDACA (0x14c2a): живёт на ПОТОЛКЕ (a.z=1, спрайт a5), ОТЦЕПЛЯЕТСЯ и ПАДАЕТ на пол (a.z→0, БЕЗ стены),
                //   на ПОЛУ (a.z=0, спрайт a0) краулит к игроку и БЫСТРО кусает; обратно на потолок — ТОЛЬКО лезя по СТЕНЕ
                //   (вертик. спрайт a1, animSt=4). НЕ умеет «всплыть» с пола без стены. ZT +0x24 высота / wall-check 0x150de. ═══
                if (ec == EC_HOPPER) {
                    if (a.state == 0) { if (los && d < 9.0) { a.state = 1; a.timer = 40; } else break; }   // ОБНАРУЖЕНИЕ
                    if (d > 16.0 && !los) { a.state = 0; a.vx = a.vy = 0; break; }                         // потерял — замирает где есть
                    int cx = (int)a.x, cy = (int)a.y;
                    bool nearWall = cellRenderWall(lvl.cellType(a.floor, cx + 1, cy)) || cellRenderWall(lvl.cellType(a.floor, cx - 1, cy))
                                  || cellRenderWall(lvl.cellType(a.floor, cx, cy + 1)) || cellRenderWall(lvl.cellType(a.floor, cx, cy - 1));
                    if (a.vz < 0.0) {                                    // ПАДЕНИЕ с потолка (быстро, без стены) — отцепился
                        a.z += a.vz; a.vz -= 0.004;                      // ускоряется вниз (гравитация)
                        moveTo(cam.px, cam.py, 0.5);                     // чуть доводит к игроку в падении
                        if (a.z <= 0.0) { a.z = 0.0; a.vz = 0.0; a.variant = 0; a.timer = 60 + (int)(enemyRng() % 50); }  // приземлился
                        a.frameT += 2; break;
                    }
                    if (a.vz > 0.0) {                                    // ЛАЗАНЬЕ ВВЕРХ по стене (медленно) — вертик. спрайт (animSt=4)
                        a.z += a.vz;
                        if (a.z >= 1.0) { a.z = 1.0; a.vz = 0.0; a.variant = 1; a.timer = 70 + (int)(enemyRng() % 50); }  // на потолке
                        a.frameT += 2; break;
                    }
                    if (a.z > 0.5) {                                     // НА ПОТОЛКЕ: краул к игроку, потом отцепиться-упасть
                        a.variant = 1;
                        moveTo(cam.px, cam.py, 0.8);
                        if (--a.timer <= 0 || d < 1.2) a.vz = -0.02;     // ОТЦЕП → падение (над игроком — сразу)
                    } else {                                            // НА ПОЛУ: краул к игроку + БЫСТРЫЙ укус
                        a.variant = 0;
                        moveTo(cam.px, cam.py, 1.0);
                        if (d < 0.6 && a.fireCd <= 0) { damagePlayer(6, cam.px, cam.py, a.x, a.y); a.fireCd = 12; a.fireAnimT = 10; }  // укус: ZT X=0x2bc→6, быстро
                        if (--a.timer <= 0) { if (nearWall) a.vz = 0.03; else a.timer = 22; }  // обратно на потолок ТОЛЬКО у стены
                    }
                    a.frameT += 2;
                    break;
                }

                // ═══ REVENANT (0x1a92a): СЕТОЧНЫЙ ПАТРУЛЬ (идёт ПРЯМО в одном из 8 направлений, на клетке поворачивает —
                //   НЕ движется к игроку!) → при ОБНАРУЖЕНИИ (LOS+радиус) РЫВОК к игроку, контакт-урон. ZT нав 0x1ac76
                //   (8 направлений по state, ±0x10, ре-навигация в центре клетки). Юзер: «идёт по линейной траектории». ═══
                if (ec == EC_RANGED_FAST) {
                    static const int RDX[8] = {1,1,0,-1,-1,-1,0,1}, RDY[8] = {0,1,1,1,0,-1,-1,-1};
                    if (a.state == 0) {                                  // ПАТРУЛЬ
                        // ZT 0x1ac4e/0x1ad72: триггер БОЯ только ВБЛИЗИ + LOS (было 6.0кл — «выскакивал издалека»);
                        // радиус чуть больше дизасм-минимума (0x180=1.5) по просьбе — комфортная дистанция обнаружения.
                        if (los && d < 2.6) { a.state = 1; }             // подошёл близко → рывок
                        else {
                            double tdx = a.aimX - a.x, tdy = a.aimY - a.y;
                            if ((a.aimX == 0 && a.aimY == 0) || std::hypot(tdx, tdy) < 0.15) {  // достиг клетки → ре-навигация
                                int cx = (int)a.x, cy = (int)a.y;
                                auto walk = [&](int dir){ int tx = cx + RDX[dir], ty = cy + RDY[dir];
                                    return !cellBlockedForEnemy(lvl.cellType(a.floor, tx, ty), 0.5, 0.5); };
                                int nd = -1;
                                if (walk(a.patrolDir) && (enemyRng() & 7)) nd = a.patrolDir;  // предпочесть ПРЯМО (линейно)
                                else { int s = (int)(enemyRng() & 7);                          // иначе выбрать новое проходимое
                                       for (int k = 0; k < 8; k++) if (walk((s + k) & 7)) { nd = (s + k) & 7; break; } }
                                if (nd < 0) { a.aimX = a.x; a.aimY = a.y; a.timer = 10; }       // заперт — стоит
                                else { a.patrolDir = (uint8_t)nd; a.aimX = (cx + RDX[nd]) + 0.5; a.aimY = (cy + RDY[nd]) + 0.5; }
                            }
                            moveTo(a.aimX, a.aimY, 0.30);                // медленно по прямой к клетке (патруль)
                            break;
                        }
                    }
                    if (d > 4.0 || (d > 2.5 && !los)) { a.state = 0; a.aimX = 0; a.aimY = 0; break; }  // оторвался → назад в патруль
                    moveTo(cam.px, cam.py, 1.0);                         // РЫВОК к игроку (полная скорость ±0x40)
                    if (d < 0.5 && a.fireCd <= 0) { damagePlayer(2, cam.px, cam.py, a.x, a.y); a.fireCd = 30; a.fireAnimT = 12; }  // ZT X=0x384→2, контакт ≤0.25кл
                    break;
                }

                // ═══ ОБЩАЯ aim-wander (дальние/гренадёр/Imp/боссы) ═══
                bool melee = (ec == EC_MELEE || ec == EC_HOPPER);
                auto reaim = [&]() {                                     // ZT 0x18334: прицел = игрок + (упреждение) + rnd
                    uint32_t r1 = enemyRng(), r2 = enemyRng();
                    double mag = melee ? 0.5 : 1.0;                      // Imp/Hydaca ±0.5кл; дальние/боссы ±1кл
                    double offx = ((int)((r1 >> 9) & 0x1ff) - 256) / 256.0 * mag;
                    double offy = ((int)((r2 >> 9) & 0x1ff) - 256) / 256.0 * mag;
                    bool lead = !melee;                                  // дальние/боссы упреждают скорость игрока
                    a.aimX = cam.px + (lead ? pvx * 6.0 : 0.0) + offx;
                    a.aimY = cam.py + (lead ? pvy * 6.0 : 0.0) + offy;
                    int base = (ec == EC_RANGED_FAST) ? 20 : 35;        // Revenant перецеливается чаще → стреляет ~2× чаще
                    a.state = 1; a.timer = base + (int)(enemyRng() % (base + 20));
                };
                if (a.state == 0) { if (los && d < 8.0) reaim(); else break; }   // ОБНАРУЖЕНИЕ (LOS + ≤8кл): до того стоит
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
                    bool crouchMiss = (player().crouchY < -2.0) && ((enemyRng() & 1) == 0);  // присед → ~50% промах (ZT питч<0)
                    int fa0 = a.fireAnimT;                       // для детекции «только что выстрелил» (fireAnimT→12)
                    switch (ec) {
                        case EC_MELEE:                                   // Imp: контакт ≤0.7кл, урон 12 (ZT d0=0x100)
                            if (d < 0.7) { damagePlayer(12, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 22; } break;
                        case EC_RANGED_FAST:                             // Revenant: контакт-рывок ≤1.6кл (ZT engage 0x12c)
                            if (d < 1.6 && !crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 30; } break;
                        case EC_RANGED:                                  // FH: HITSCAN ≤4кл, falloff 16−дист·4
                            if (d < 4.0 && !crouchMiss) { damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 55; } break;
                        case EC_GRENADIER:                               // Sgt(человек)/FH-SF: ЧАЩЕ hitscan, ИНОГДА НАСТОЯЩАЯ граната (~1/4)
                            if (d < 5.0) {
                                if (d < 4.0 && !crouchMiss) damagePlayer(zdamage(d), cam.px, cam.py, a.x, a.y);  // выстрел
                                if (d < 9.0 && (enemyRng() & 3) == 0) spawnGrenade(a.x, a.y, a.floor, ux, uy, 1);  // граната как обычная (дуга+отскок+фитиль), детонир. у игрока
                                a.fireAnimT = 12; a.fireCd = 55;
                            } break;
                        case EC_BOSS_PROJ:                               // Boss1: САМОНАВОДЯЩИЙСЯ снаряд
                            if (d < 12.0) { spawnEnemyShot(a.x, a.y, a.floor, ux, uy, 1); a.fireAnimT = 12; a.fireCd = 60; } break;
                        case EC_BOSS_MELEE: {                            // Boss3(0x6A) контакт X=0x100→12; Boss2(0x6B) даль(4кл) X=0x1f4→9
                            int bd = (a.srcType == 0x6A) ? 12 : 9; double br = (a.srcType == 0x6A) ? 1.0 : 4.0;
                            if (d < br) { damagePlayer(bd, cam.px, cam.py, a.x, a.y); a.fireAnimT = 12; a.fireCd = 45; } } break;
                        default: break;
                    }
                    if (a.fireAnimT > fa0 && a.floor == cam.floor &&        // ВРАГ ТОЛЬКО ЧТО ВЫСТРЕЛИЛ → звук выстрела врага (ранговые)
                        (ec == EC_RANGED || ec == EC_RANGED_FAST || ec == EC_GRENADIER || ec == EC_BOSS_PROJ))
                        snd::ev(snd::SFX_ENEMY_FIRE);
                }
                break;
            }
            case AT_ENEMY_SHOT: {                        // снаряд врага (Boss1 самонавод / FH-SF граната): летит → урон / искра
                if (a.state == 1 && a.floor == cam.floor) {             // САМОНАВЕДЕНИЕ (Boss1 0x15f6e): довод курса к игроку
                    double hx = cam.px - a.x, hy = cam.py - a.y, hd = std::hypot(hx, hy);
                    if (hd > 0.01) { double sp = 0.16;                  // плавный поворот к цели, держим скорость
                        a.vx = (a.vx * 0.8 + hx / hd * sp * 0.2); a.vy = (a.vy * 0.8 + hy / hd * sp * 0.2); }
                } else if (a.state == 2) {                              // ГРАНАТА (FH-SF): дуга — z вверх + гравитация
                    a.z += a.vz; a.vz -= 0.004; if (a.z < 0) a.z = 0;
                }
                if (a.floor == cam.floor && gameDist(cam.px - a.x, cam.py - a.y) < 0.5) {     // достал игрока (октаг. ≤0x40)
                    damagePlayer(9, cam.px, cam.py, a.x, a.y); a.active = false; break;       // снаряд: урон 9 (ZT d0=0x1f4→16−7)
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
                if (a.timer > 0 && std::abs(a.vx) + std::abs(a.vy) > 0.004) {
                    double nx = a.x + a.vx, ny = a.y + a.vy;
                    if (!cellBlockedForEnemy(lvl.cellType(a.floor, (int)nx, (int)a.y), nx - (int)nx, a.y - (int)a.y)) a.x = nx;
                    if (!cellBlockedForEnemy(lvl.cellType(a.floor, (int)a.x, (int)ny), a.x - (int)a.x, ny - (int)ny)) a.y = ny;
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
            case AT_BLOOD: {                                // КРОВЬ: лёт=тайл61 мелкий на высоте z; пятно=тайл62, тускнеет (↓размер)
                if (a.state == 0) { int sz = 2; int c = (int)(8 - a.z * 16);  // летящая капля
                    fx.push_back({a.x, a.y, a.floor, A_BLOOD_FLY, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz}); }
                else { int life = 48 - a.timer; int sz = (life > 12) ? 4 : (life > 4 ? 3 : 2);  // пятно: уменьшается к концу
                    int c = (a.state == 2) ? 8 : (int)(8 - a.z * 16);        // пол: на полу (c=8); стена: на высоте z
                    fx.push_back({a.x, a.y, a.floor, A_BLOOD_SPLAT, (int8_t)(c - sz/2), (uint8_t)sz, (uint8_t)sz}); }
                break; }
            case AT_FLAME: { int sz = 3 + a.timer / 3;     // пламя в полёте, мелкое, квадрат, к концу мельчает
                fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)(-sz/2), (uint8_t)sz, (uint8_t)sz}); break; }
            case AT_FIRE: {                                // ОГОНЬ на полу: 2-КАДРОВАЯ анимация (ZT 0x14292: тайл N / N+1 по счётчику&1)
                int frame = (a.frameT >> 2) & 1;          // чередование 2 кадров (тайл 0 / тайл 1) — «живое» пламя
                uint8_t t = frame ? (uint8_t)(A_FIRE_TILE + 1) : A_FIRE_TILE;
                int sz = 8 + frame;
                if (a.fireCd > 0) sz = sz * a.fireCd / 10; // ЗАТУХАНИЕ от пены: огонь УМЕНЬШАЕТСЯ за ~10 кадров
                if (sz < 1) sz = 1; int c = 8 - sz;        // низом на полу, лёгкий пульс размера между кадрами
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
                else fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)-2, 3, 3});
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
                    if (a.srcType == 0x65 && a.vz > 0.0 && a.hitT <= 0 && !A.climb.empty()) animSt = 4;  // Hydaca лезет по стене → вертик. спрайт
                    uint8_t dir = (uint8_t)enemyDirIndex(a.vx, a.vy, cam.px - a.x, cam.py - a.y, A.walkDirs);  // поворот по углу
                    int nf = (animSt == 1) ? (int)A.fire.size() : (animSt == 2) ? (int)A.hit.size()
                                           : (animSt == 4) ? (int)A.climb.size()
                                           : (int)A.walk[dir < A.walkDirs ? dir : 0].size();
                    if (nf < 1) nf = 1;
                    uint8_t efr = (animSt == 1) ? (uint8_t)(((12 - a.fireAnimT) / 3) % nf)   // стрельба: прогон кадров
                                : (animSt == 2) ? (uint8_t)((a.hitT / 3) % nf)                // стаггер (hitT растёт)
                                : (nf > 1 ? (uint8_t)((a.frameT / 6) % nf) : 0);              // ходьба: по движению
                    fx.push_back({a.x, a.y, a.floor, 0, (int8_t)-8, 16, 7, slot, efr, false, (float)a.z, dir, animSt, a.variant});
                } else
                    fx.push_back({a.x, a.y, a.floor, a.tile, (int8_t)-8, 16, 7});
                break; }
            case AT_CORPSE: {                             // труп: последний кадр death-анима (лежит); нет death → сплющ. ходьба
                int slot = enemyGfxSlot(a.srcType);
                if (slot >= 0 && slot < 16 && g_enemyAnim[slot].ok) {
                    EnemyAnimSet& A = (a.variant && g_enemyAnimVar2[slot].ok) ? g_enemyAnimVar2[slot] : g_enemyAnim[slot];
                    bool hasDeath = !A.death.empty();
                    uint8_t efr = hasDeath ? (uint8_t)(A.death.size() - 1) : 0;
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
