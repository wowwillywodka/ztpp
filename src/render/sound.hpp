#pragma once
// ── ЗВУК ZT через ЭМУЛЯТОР YM2612 (Nuked-OPN2) ───────────────────────────────────────────────────
// Звук в оригинале — движок GEMS на чипе Yamaha YM2612: и эффекты, и музыка проигрываются ИМ.
// Здесь воспроизводим НАСТОЯЩИМ чипом (vendored src/opn2/ym3438.c, nukeykt, LGPL 2.1):
//   • PCM (ударные/импакты) = DAC-сэмплы GEMS (таблица @0x5E51C, u8) → DAC-регистр 0x2A канала 6.
//   • FM (синт-эффекты: двери/лифт/меню/подбор/писк) = FM-патчи GEMS (банк @0x5A804, 39-байт инстр.).
// Декод патча/частоты — порт mdgfx/gems_music.py + opn2_chip.py (выверено на музыке ZT).
//
// МОДУЛЬ src/render: заголовок = состояние (синглтоны)/структуры/крошечные хелперы + SFX-карты (inline);
// тела «действий» (чип-запись, GEMS-секвенсер, genNative/mix, воспроизведение, load) — в sound.cpp.
#include "rom.hpp"
#include "tuning.hpp"
#include <SDL.h>
#include <vector>
#include <cstdint>
#include <cmath>

extern "C" {
#include "opn2/ym3438.h"
}

namespace snd {

// ── НАСТРОЙКИ ЧИПА ──
static const double YM_CLOCK   = 7670454.0;            // NTSC мастер-такт YM2612
static const double NATIVE_RATE = YM_CLOCK / 144.0;    // 53267 Гц — один сэмпл = сумма 24 тактов чипа
static const int    FM_SFX_CH  = 6;                    // все 6 FM-голосов в общем пуле (ROM: DAC отбирает phys6 только НА ВРЕМЯ PCM)

inline ym3438_t& chip()        { static ym3438_t c; return c; }
inline SDL_AudioDeviceID& dev(){ static SDL_AudioDeviceID d = 0; return d; }
inline int& devRate()          { static int r = 44100; return r; }
inline bool& inited()          { static bool b = false; return b; }

// Запись регистра YM2612 с тактированием.  → sound.cpp
void wr(int port, int addr, int data);

// ── DAC-СЭМПЛ (PCM) ──
struct Sample { std::vector<uint8_t> pcm; double rate = 10500; };   // u8 unsigned, частота воспроизв.
inline std::vector<Sample>& samples() { static std::vector<Sample> v; return v; }
inline int count() { return (int)samples().size(); }

// ── FM-ПАТЧ (инструмент GEMS, 39 байт) → регистры YM2612 ──
struct FmPatch {
    bool ok = false; uint8_t alg = 0, fb = 0, lfo = 0, key = 0xF, B0 = 0, B4 = 0xC0;
    uint8_t op[4][7];                                  // [op][0..6] = регистры 0x30,40,50,60,70,80,90
};
inline const int* opRegList() { static const int r[7] = {0x30,0x40,0x50,0x60,0x70,0x80,0x90}; return r; }
inline const int* opOffset()  { static const int o[4] = {0,8,4,12}; return o; }    // логич.op → смещение регистра
inline const uint8_t* algCarriers() { static const uint8_t c[8] = {0x8,0x8,0x8,0x8,0xA,0xE,0xE,0xF}; return c; }  // несущие по алгоритму

inline std::vector<FmPatch>& patches() { static std::vector<FmPatch> v; return v; }
inline std::vector<uint8_t>& patchTypes() { static std::vector<uint8_t> v; return v; }  // тип патча: 0 FM, 1 DAC, 2 PSG, 3 NOISE

FmPatch decodeFmPatch(const uint8_t* p);                       // → sound.cpp
void noteToFnum(double note, int& block, int& fnum);           // → sound.cpp (MIDI-нота → block,fnum)

// ── ГОЛОСА (состояние в аудио-потоке) ──
struct FmVoice { bool active = false; int ch = 0; int patch = -1; long keyOff = 0, freeAt = 0; long started = 0; };
struct DacVoice { bool active = false; const Sample* s = nullptr; double pos = 0, step = 1; };
// ── SN76489 PSG NOISE (NOISE-патчи 0x97 выстрел-крэш, 0x15) ── ВЕРНО ПО РАЗБОРУ GEMS Z80-драйвера:
//   16-бит LFSR (белый = бит0^бит3, Sega tap $0009), ПРОГРАММНАЯ GEMS ADSR-огибающая (psg_service $0066)
//   ведёт 4-бит аттенюацию. Параметры из сырых байт инструмента (см. psgInsts).
struct PsgInst { uint8_t b[7] = {0}; };
inline std::vector<PsgInst>& psgInsts() { static std::vector<PsgInst> v; return v; }   // сырые байты каждого патча

// SN76489: 4-бит аттенюация -> амплитуда (2 dB/шаг; 0=громко, 15=выкл)
inline double psgAmp(int att4) {
    static double t[16]; static bool ini = false;
    if (!ini) { for (int i = 0; i < 15; ++i) t[i] = std::pow(10.0, -0.1 * i); t[15] = 0.0; ini = true; }
    return t[att4 & 15];
}
// тумблеры (консоль noiseenv/noisepeak/noiserate)
inline double& noiseEnvHz()  { static double h = 60.0;  return h; }   // ⭐ГЕЙМС-тик = 60 Гц (VBlank), ИЗМЕРЕНО из MAME
inline double& noisePeak()   { static double p = 200.0; return p; }   // пик амплитуды noise-голоса (SN76489 тише YM2612)
inline int&    noiseRateOv() { static int r = -1;       return r; }   // >0: ручной период LFSR (native), иначе из ctrl

struct NoiseVoice {
    bool active = false;
    uint16_t lfsr = 0x8000;                    // 16-бит сдвиговый регистр SN76489
    bool white = true;                         // FB: белый / периодический
    double shiftAcc = 0, shiftPeriod = 4;      // делитель LFSR-клока в native-сэмплах
    int cur = 1;                               // текущий выход LFSR (+-1)
    int att = 255, phase = 0;                  // GEMS ADSR: att 0..255, фаза 0i/1a/2d/3s/4r
    int atk = 0xff, dec = 9, sus = 240, rel = 15, tgt = 0;
    double envAcc = 0; long life = 0;          // тик огибающей / предохранитель по времени
};
inline FmVoice*   fmv()   { static FmVoice  v[FM_SFX_CH]; return v; }
inline DacVoice*  dacv()  { static DacVoice v[6];         return v; }
inline NoiseVoice& noiseV(){ static NoiseVoice n; return n; }
inline long& nativeClock() { static long t = 0; return t; }   // счётчик нативных сэмплов (аудио-поток)

// FM-голос ch → (part 0/1, ch-offset 0..2, key-маска канала для 0x28).
inline void chAddr(int ch, int& part, int& cofs, int& keych) {
    part = ch < 3 ? 0 : 1; cofs = ch % 3; keych = part ? (cofs + 4) : cofs;
}
void loadPatch(int ch, const FmPatch& fp);                     // → sound.cpp

// ── GEMS-СЕКВЕНСЕР (МУЗЫКА): порт mdgfx/gems_player.py (реалтайм, в аудио-потоке) ──
static const int GEMS_MAX_CH = 16;
struct GemsChan {
    int  pos = 0; int wait = 0, delayv = 0, duration = 0;
    bool wasdelay = false, wasduration = false, done = true;
    int  patch = -1, patchType = -1, voice = -1, note = -1; long noteOff = -1;
    int  loopRet[8]; int loopRem[8]; int loopDepth = 0; int volume = 0;   // ⭐GEMS: АТТЕНЮАЦИЯ канала 0..0x7F (0=полная громкость; данные 0x72/05=0..40)
    int  psgV = -1;                                              // PSG-тон голос (музыка), −1 = нет
};
// ── PSG ТОН для МУЗЫКИ (SN76489 ch0-2 + GEMS ADSR из Z80-драйвера psg_service $0066) ──
// Патч: b[2]=atkRate b[3]=susLevel b[4]=atkTgt b[5]=decRate b[6]=relRate (8-бит атт: 0=громко, 255=тишина).
// Меандр: период = NATIVE_RATE/(2·freq); ADSR тикает на noiseEnvHz (60 Гц), вывод = psgAmp(att>>4).
struct PsgTone {
    bool active = false; int phase = 0; int owner = -1;         // owner = GEMS-канал
    double att = 255, atk = 16, dec = 8, sus = 0, tgt = 0, rel = 16;
    double halfPeriod = 100, acc = 0, envAcc = 0; int cur = 1; int volAtt = 0;
};
inline PsgTone* psgTones() { static PsgTone v[3]; return v; }
struct GemsVoice { int owner = -1; int patch = -1; long last = -1, off = 0; int vol = -1; };
// ⭐ЕДИНЫЙ ПУЛ 6 FM-КАНАЛОВ (GEMS Z80 $17e2, трассировка 2026-07-16): канал = {busy, prio, sfx?}.
// Алгоритм note-on: (1) СВОЙ канал переиспользуется; (2) свободный (LRU); (3) свободных нет → КРАЖА у
// занятого с МИНИМАЛЬНЫМ приоритетом, ТОЛЬКО если prio запроса ≥ его; (4) иначе ОТКАЗ — звук НЕ играет
// (музыку не рвёт). Приоритеты: музыка=1, SFX=2 (SFX крадёт у музыки один худший голос; музыка у SFX — нет).
inline int* chanPrio() { static int p[6] = {0,0,0,0,0,0}; return p; }   // приоритет ЗАНЯТОГО канала (0=свободен)
// ROM $17e2: свободный голос, чей ПОСЛЕДНИЙ владелец (запись +3) == запросивший канал, берётся сразу
// (музыкальный канал липнет к «своему» голосу). Владельцы: 0..15 = каналы музыки, 16+ = SFX (по патчу).
inline int* voiceLastOwner() { static int p[6] = {-1,-1,-1,-1,-1,-1}; return p; }
// ROM бит5 записи $17b1: пока DAC играет PCM, FM-голос 5 (phys6) исключён из аллокации (DAC глушит FM ch6).
inline bool& dacLocked() { static bool b = false; return b; }
inline bool& dacOn()     { static bool b = false; return b; }   // текущее состояние рег. $2B (DAC enable)
struct GemsMusic {
    bool active = false; bool paused = false; int song = -1; int tempo = 120; double acc = 0;
    long tick = 0; int nch = 0;
    bool ptr3 = false;         // ⭐указатели каналов песни: false=2 байта (ZT), true=3 (GEMS-флаг «3», ZTU)
    GemsChan ch[GEMS_MAX_CH]; GemsVoice voice[6];
    std::vector<uint8_t> seq;                                    // копия банка секвенций (окно 64К)
};
inline GemsMusic& music() { static GemsMusic m; return m; }
inline int& musicTranspose() { static int t = 12; return t; }    // как gems_player render_song (выверено)
inline double& musicVolume() { static double v = 1.0; return v; }

// GEMS-секвенсер (реалтайм в аудио-потоке).  → sound.cpp
void gemsLoadPatchVoice(int v, int patchIdx);
void gemsKeyOffVoice(int v);
void gemsNoteOffCh(GemsMusic& m, GemsChan& c);
int  gemsAllocFm(int reqPrio, int ownerId);
void gemsNoteOn(GemsMusic& m, int chIdx, int note);
void gemsDelayAdd(GemsChan& c);
void gemsAdvance(GemsMusic& m, int chIdx);
void gemsTick(GemsMusic& m);
void musicStopLocked();

// ── ГЕНЕРАЦИЯ/МИКС (аудио-поток) ──  → sound.cpp
void genNative(int& outL, int& outR);
void mix(void*, Uint8* stream, int len);

// ── ВОСПРОИЗВЕДЕНИЕ ──  → sound.cpp
void play(int id);
void playNoise(int patchIdx = -1);
void playFm(int patchIdx, int note = 60, double hold = 0.14, double tail = 0.45);
inline int patchCount() { return (int)patches().size(); }

// ── МУЗЫКА: запуск песни (GEMS-секвенция 0..8) ──
inline int musicSongCount() {
    const std::vector<uint8_t>& d = music().seq;
    if (d.size() < 2) return 0;
    return (d[0] | (d[1] << 8)) / 2;
}
void musicStop();                                              // → sound.cpp
void musicSetPaused(bool p);                                   // ⭐пауза GEMS (ROM cmd 0x0D/0x0C: ESC-меню/Tab-карта)
bool renderMusicWav(int song, double seconds, const char* path);   // отладка: трек → WAV (headless)
bool musicPlay(int song, int tempo = -1);                      // → sound.cpp (tempo из SFX-таблицы p2, cmd5/0e1d)
inline int musicCurrent() { return music().active ? music().song : -1; }

// ── SFX-ДЕСКРИПТОРЫ ZT (таблица @0xc5eb4, 3 байта/запись: type,p1,p2) ──
//   type 0 = МУЗЫКА (секвенция p1); type 1 = FM-патч p1 + НОТА p2 (синт-SFX);
//   type 2/3 = DAC-СЭМПЛ p1 (перкуссия: выстрелы/взрывы/удары).
struct SfxDesc { uint8_t type = 0, p1 = 0, p2 = 0; };
inline std::vector<SfxDesc>& sfxTable() { static std::vector<SfxDesc> v; return v; }
inline int sfxCount() { return (int)sfxTable().size(); }
inline int& sfxTranspose() { static int t = 12; return t; }  // ⭐ +12 ДОКАЗАНО по Z80-драйверу (block=note/12). Консоль `sfxtr`.
inline double& sfxHold() { static double h = 0.80; return h; }   // FM-SFX: держать ноту (сек). Тюнинг Sound Test [/]
inline double& sfxTail() { static double t = 0.50; return t; }   // FM-SFX: хвост после key-off (сек)
inline bool& musicEnabled() { static bool b = true; return b; }   // авто-музыка по игровым sound-id (type0)
// ⭐ DAC-индекс сэмпла = СВАП вокруг 0x30 (ВЕРИФИЦ. по Z80-драйверу GEMS @0x12f9): sample = (p1>=0x30)?p1−0x30:p1+0x30.
inline bool& dacRemap48() { static bool b = true; return b; }
inline int dacSampleIdx(const SfxDesc& d) { if (!dacRemap48()) return d.p1; return (d.p1 >= 0x30) ? (d.p1 - 0x30) : (d.p1 + 0x30); }

bool playSfx(int id);                                          // → sound.cpp (по игровому id)
void playElevatorHum();                                        // → sound.cpp (лифт: 0x6f одной длинной нотой)
void playSfxNote(int id, int note);                            // → sound.cpp (Sound Test: явная нота)
void stopAllSfx();                                             // → sound.cpp

// ── ЗАГРУЗКА: таблица DAC-сэмплов + банк FM-патчей + секвенции + SFX-таблица + чип/аудио ──
// GEMS-банки per-build: ZT samp 0x5E51C / patch 0x5A804 / seq 0x5B0CC / SFX 0xC5EB4;
// ZTU 0x2EE77 / 0x2981E / 0x2A288 / 0x96848 (GEMS-init пуши @0x96492, sfx = lea (pc) @0x96a7a — verified).
void load(const Rom& rom, size_t sampBase = 0x5E51C, size_t patchBase = 0x5A804,
          size_t seqBase = 0x5B0CC, size_t sfxBase = 0xC5EB4);   // → sound.cpp

// ── СОБЫТИЯ → ИГРОВОЙ SFX-ID (выверено по дизасм-вызовам jsr 0xd740/0xd760 с d0=id) ──
enum Ev { SFX_HANDGUN, SFX_SHOTGUN, SFX_LASER, SFX_PULSE, SFX_ROCKET, SFX_GRENADE, SFX_EXPLOSION,
          SFX_FLAME, SFX_FOAM, SFX_PICKUP, SFX_HURT, SFX_DOOR, SFX_SWITCH, SFX_ENEMY_HIT,
          SFX_ENEMY_FIRE, SFX_ENEMY_DEATH, SFX_WALL, SFX_ELEVATOR, SFX_GRENADE_BOUNCE, SFX_MENU, SFX__COUNT };
// Карта событие→id ВЫВЕРЕНА по annotated/zt (weapons.asm 12a70 / objects_items / enemy_ai), 2026-07-10.
inline int* evMap() { static int m[SFX__COUNT] = {
    /*HANDGUN  */ 0x19, /*SHOTGUN */ 0x1d, /*LASER  */ 0x1e, /*PULSE  */ 0x1e, /*ROCKET */ 0x96,   // fire-хендлеры 12eac/1303c/12f62/130de/12ff4
    /*GRENADE  */ 0x1b, /*EXPLOSION*/0x8a, /*FLAME  */ 0x96, /*FOAM   */ 0x6d, /*PICKUP */ 0x69,  // GRENADE=бросок 0x1b(12e7c); FLAME=0x96(draw 12828); FOAM=огнетушитель 0x6d; PICKUP=COLLECTED 0x69
    /*HURT     */ 0x68, /*DOOR    */ 0x67, /*SWITCH */ 0x6b, /*ENEMY_HIT*/ 0x68,                  // DOOR=0x67 (b35c/15c74); HURT/ENEMY_HIT не триггерятся
    /*ENEMY_FIRE*/ 0x1b, /*ENEMY_DEATH*/ 0x2d, /*WALL*/ 0x3b, /*ELEVATOR*/ 0x6f, /*GRENADE_BOUNCE*/ 0x18, /*MENU*/ 0x6b }; return m; }
    // ⚠ ещё уточнить: GRENADE(fire 12e7c)/FLAME(13024)/ENEMY_FIRE(per-enemy)/WALL(0x3b?=gameover-взрыв)
inline void ev(Ev e) { if (e < 0 || e >= SFX__COUNT) return; playSfx(evMap()[e]); }

// ── PER-ENEMY звуки (celltype врага; выверено по annotated/zt enemy_ai.asm) ──
inline int enemyDeathSfx(uint8_t ct) {
    switch (ct) {
        case 0x2A: return 0x2d;  // Former Human      (184ea)
        case 0x2B: return 0x23;  // Imp               (189ea)
        case 0x65: return 0x25;  // Hydaca            (15654)
        case 0x68: return 0x2b;  // Pink Dog          (19800)
        case 0x69: return 0x21;  // FH Special Forces (19e48)
        case 0x29: return 0x1f;  // FH Sergeant       (1b498)
        case 0x67: return 0x29;  // Boss 1            (1938c)
        case 0x6B: return 0x31;  // Boss 2            (18eac)
        case 0x6A: return 0x2f;  // Boss 3            (1a6e6)
        case 0x66: return 0x27;  // Revenant          (1aaea)
    }
    return 0x2d;
}
inline int enemyFireSfx(uint8_t ct) {
    switch (ct) {
        case 0x67: return 0x96;  // Boss1 — снаряд (19334)
        case 0x6B: case 0x6A: case 0x66: return 0x1e;  // Boss2/3/Revenant — дальний лазер
        case 0x2B: return 0x94;  // Imp — мили-удар (189de)
        case 0x65: return 0x93;  // Hydaca — мили-удар (14ef2)
    }
    return 0x1b;  // hitscan-люди (FH 2A / FH-SF 69 / Sgt 29) и Dog 68
}
// hitscan-враг → при ПОПАДАНИИ по игроку играет 0x68 (+0x97 шум). Только эти типы.
inline bool enemyWoundsPlayer(uint8_t ct) { return ct == 0x2A || ct == 0x69 || ct == 0x29 || ct == 0x67; }
// Звук ПОЯВЛЕНИЯ врага — из objdef байт 0x25 (ROM @0xAB0C+). Уникален по типу.
inline int enemyAppearSfx(uint8_t ct) {
    switch (ct) {
        case 0x2B: return 0x22;  // Imp        (objdef@ab58+0x25)
        case 0x65: return 0x24;  // Hydaca     (ab7e)
        case 0x66: return 0x26;  // Revenant   (aba4)
        case 0x67: return 0x28;  // Boss 1     (abca)
        case 0x68: return 0x2a;  // Pink Dog   (abf0)
        case 0x69: return 0x20;  // FH-SF      (ac16)
        case 0x2A: case 0x29: return 0x2c;  // FH / Sergeant (ab32/ab0c)
        case 0x6A: return 0x2e;  // Boss 3     (ac3c)
        case 0x6B: return 0x30;  // Boss 2     (ac62)
    }
    return -1;
}

}  // namespace snd
