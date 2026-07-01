#pragma once
// ── ЗВУК ZT через ЭМУЛЯТОР YM2612 (Nuked-OPN2) ───────────────────────────────────────────────────
// Звук в оригинале — движок GEMS на чипе Yamaha YM2612: и эффекты, и музыка проигрываются ИМ.
// Здесь воспроизводим эффекты НАСТОЯЩИМ чипом (vendored src/opn2/ym3438.c, nukeykt, LGPL 2.1):
//   • PCM (ударные/импакты) = DAC-сэмплы GEMS (таблица @0x5E51C, u8) → пишутся в DAC-регистр 0x2A
//     канала 6 (как в железе: GEMS DACPlay, частота = YM/144/(flags&0xF)).
//   • FM (синт-эффекты: двери/лифт/меню/подбор/писк) = FM-патчи GEMS (банк @0x5A804, 39-байт инстр.)
//     грузятся в регистры голоса и берутся одной нотой (как ZT триггерит FM-SFX патчем+нотой).
// Декод патча/частоты — порт mdgfx/gems_music.py + opn2_chip.py (выверено на музыке ZT).
// Колбэк SDL тактирует чип на НАТИВНОЙ частоте 53267 Гц (=master/144) и ресемплит в частоту устройства.
// Музыка (9 секвенций @0x5B0CC) — GEMS-секвенсер, ОТДЕЛЬНЫЙ проход (юзер: «сперва звуки, потом музыка»).
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

// ── НАСТРОЙКИ ЧИПА ───────────────────────────────────────────────────────────────────────────────
static const double YM_CLOCK   = 7670454.0;            // NTSC мастер-такт YM2612
static const double NATIVE_RATE = YM_CLOCK / 144.0;    // 53267 Гц — один сэмпл = сумма 24 тактов чипа
static const int    FM_SFX_CH  = 5;                    // FM-голоса 0..4 под SFX (5-й/ch6 отдан DAC)

inline ym3438_t& chip()        { static ym3438_t c; return c; }
inline SDL_AudioDeviceID& dev(){ static SDL_AudioDeviceID d = 0; return d; }
inline int& devRate()          { static int r = 44100; return r; }
inline bool& inited()          { static bool b = false; return b; }

// Запись регистра YM2612 с тактированием (как wrap.c: addr→24 такта→data→24 такта, иначе чип теряет запись).
inline void wr(int port, int addr, int data) {
    Bit16s b[2];
    OPN2_Write(&chip(), (Bit32u)(port ? 2 : 0),       (Bit8u)addr); for (int i = 0; i < 24; ++i) OPN2_Clock(&chip(), b);
    OPN2_Write(&chip(), (Bit32u)((port ? 2 : 0) | 1), (Bit8u)data); for (int i = 0; i < 24; ++i) OPN2_Clock(&chip(), b);
}

// ── DAC-СЭМПЛ (PCM) ──────────────────────────────────────────────────────────────────────────────
struct Sample { std::vector<uint8_t> pcm; double rate = 10500; };   // u8 unsigned, частота воспроизв.
inline std::vector<Sample>& samples() { static std::vector<Sample> v; return v; }
inline int count() { return (int)samples().size(); }

// ── FM-ПАТЧ (инструмент GEMS, 39 байт) → регистры YM2612 ─────────────────────────────────────────
// Слоты данных лежат в порядке 1,3,2,4; логич.OP[i] = слот ((i<<1)|(i>>1))&3. Регистры — как gems_music.fm_patch_to_ym2612.
struct FmPatch {
    bool ok = false; uint8_t alg = 0, fb = 0, lfo = 0, key = 0xF, B0 = 0, B4 = 0xC0;
    uint8_t op[4][7];                                  // [op][0..6] = регистры 0x30,40,50,60,70,80,90
};
inline const int* opRegList() { static const int r[7] = {0x30,0x40,0x50,0x60,0x70,0x80,0x90}; return r; }
inline const int* opOffset()  { static const int o[4] = {0,8,4,12}; return o; }    // логич.op → смещение регистра
inline const uint8_t* algCarriers() { static const uint8_t c[8] = {0x8,0x8,0x8,0x8,0xA,0xE,0xE,0xF}; return c; }  // несущие по алгоритму

inline std::vector<FmPatch>& patches() { static std::vector<FmPatch> v; return v; }

inline FmPatch decodeFmPatch(const uint8_t* p) {
    FmPatch fp;
    if (p[0] != 0) return fp;                          // type!=0 → не FM (DAC/PSG/NOISE)
    uint8_t b1 = p[1], b3 = p[3], b4 = p[4];
    fp.lfo = (uint8_t)((((b1 >> 3) & 1) << 3) | (b1 & 7));            // reg22 = LFO_on<<3 | LFO_val
    fp.B0  = (uint8_t)((((b3 >> 3) & 7) << 3) | (b3 & 7));            // FB<<3 | ALG
    fp.B4  = (uint8_t)(((b4 >> 7) << 7) | (((b4 >> 6) & 1) << 6) | (((b4 >> 4) & 3) << 4) | (b4 & 7));
    fp.alg = (uint8_t)(b3 & 7); fp.fb = (uint8_t)((b3 >> 3) & 7);
    for (int i = 0; i < 4; ++i) {
        int slot = ((i << 1) | (i >> 1)) & 3;
        const uint8_t* o = p + 5 + slot * 6;
        uint8_t d0 = o[0], d1 = o[1], d2 = o[2], d3 = o[3], d4 = o[4], d5 = o[5];
        fp.op[i][0] = (uint8_t)((((d0 >> 4) & 7) << 4) | (d0 & 0xF));  // 0x30 DT<<4|MUL
        fp.op[i][1] = (uint8_t)(d1 & 0x7F);                            // 0x40 TL
        fp.op[i][2] = (uint8_t)(((d2 >> 6) << 6) | (d2 & 0x1F));       // 0x50 RS<<6|AR
        fp.op[i][3] = (uint8_t)(((d3 >> 7) << 7) | (d3 & 0x1F));       // 0x60 AM<<7|DR
        fp.op[i][4] = (uint8_t)(d4 & 0x1F);                            // 0x70 D2R
        fp.op[i][5] = (uint8_t)(((d5 >> 4) << 4) | (d5 & 0xF));        // 0x80 SL<<4|RR
        fp.op[i][6] = 0;                                               // 0x90 SSG-EG
    }
    fp.key = (uint8_t)(p[37] & 0xF);
    fp.ok = true;
    return fp;
}

// MIDI-нота → (block,fnum) YM2612 (порт opn2_chip.note_to_fnum_block).
inline void noteToFnum(double note, int& block, int& fnum) {
    double freq = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    block = 4;
    while (block < 7) { double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK; if (f < 0x800) break; ++block; }
    while (block > 0) { double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK; if (f >= 0x400) break; --block; }
    double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK;
    fnum = (int)std::lround(f); if (fnum < 0) fnum = 0; if (fnum > 0x7FF) fnum = 0x7FF;
}

// ── ГОЛОСА (состояние в аудио-потоке) ────────────────────────────────────────────────────────────
struct FmVoice { bool active = false; int ch = 0; int patch = -1; long keyOff = 0, freeAt = 0; long started = 0; };
struct DacVoice { bool active = false; const Sample* s = nullptr; double pos = 0, step = 1; };
inline FmVoice*  fmv()  { static FmVoice  v[FM_SFX_CH]; return v; }
inline DacVoice* dacv() { static DacVoice v[6];         return v; }
inline long& nativeClock() { static long t = 0; return t; }   // счётчик нативных сэмплов (аудио-поток)

// FM-голос i → (part 0/1, ch-offset 0..2, key-маска канала для 0x28).
inline void chAddr(int ch, int& part, int& cofs, int& keych) {
    part = ch < 3 ? 0 : 1; cofs = ch % 3; keych = part ? (cofs + 4) : cofs;
}
inline void loadPatch(int ch, const FmPatch& fp) {
    int part, cofs, keych; chAddr(ch, part, cofs, keych);
    for (int op = 0; op < 4; ++op) { int base = opOffset()[op] + cofs;
        for (int r = 0; r < 7; ++r) wr(part, opRegList()[r] + base, fp.op[op][r]); }
    wr(part, 0xB0 + cofs, fp.B0); wr(part, 0xB4 + cofs, fp.B4); wr(0, 0x22, fp.lfo);
}

// ── ГЕНЕРАЦИЯ ОДНОГО НАТИВНОГО СЭМПЛА (аудио-поток): DAC-подкормка + key-off/free FM + 24 такта ──
// DAC-запись (0x2A) ВПИСАНА в 24 такта сэмпла (addr→12 тактов→data→12 тактов) — без лишних тактов,
// чтобы не «съедать» время чипа (иначе огибающие FM спешат). Тяжёлый wr() — только для редких
// событий (загрузка патча/нота/key-off), там лишние такты пренебрежимы (как в офлайн-рендере).
inline void genNative(int& outL, int& outR) {
    long t = ++nativeClock();
    // FM SFX: расписание key-off / освобождение голоса
    for (int i = 0; i < FM_SFX_CH; ++i) { FmVoice& v = fmv()[i]; if (!v.active) continue;
        int part, cofs, keych; chAddr(v.ch, part, cofs, keych);
        if (v.keyOff && t >= v.keyOff) { wr(0, 0x28, keych); v.keyOff = 0; }   // key-off (маска операторов 0)
        if (t >= v.freeAt) v.active = false;
    }
    // DAC PCM: смешать активные голоса в один DAC-байт (как software-mix перед DAC)
    int dacActive = 0, acc = 0;
    for (int i = 0; i < 6; ++i) { DacVoice& d = dacv()[i]; if (!d.active) continue;
        size_t idx = (size_t)d.pos;
        if (idx >= d.s->pcm.size()) { d.active = false; continue; }
        acc += (int)d.s->pcm[idx] - 128; d.pos += d.step; ++dacActive;
    }
    int dacByte = 0x80;                                       // центр = тишина
    if (dacActive) { int v = acc; if (v > 127) v = 127; if (v < -128) v = -128; dacByte = (v + 128) & 0xFF; }
    // 24 такта чипа = один сэмпл, с впис. DAC-записью (addr/data разнесены тактами для консумации FIFO)
    Bit16s b[2]; long l = 0, r = 0; int s = 0;
    OPN2_Write(&chip(), 0, 0x2A);                             // DAC addr
    for (; s < 12; ++s) { OPN2_Clock(&chip(), b); l += b[0]; r += b[1]; }
    OPN2_Write(&chip(), 1, (Bit8u)dacByte);                  // DAC data
    for (; s < 24; ++s) { OPN2_Clock(&chip(), b); l += b[0]; r += b[1]; }
    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    outL = (int)l; outR = (int)r;
}

// SDL callback: ресемпл NATIVE→devRate (линейный) + громкость/мьют.
inline void mix(void*, Uint8* stream, int len) {
    int16_t* out = (int16_t*)stream;
    int frames = len / (int)(2 * sizeof(int16_t));            // стерео S16
    // мастер-гейн: один FM-голос/DAC чипа тихий (пик ~800/32767), поднимаем; soundVolume сверху, клип защищает
    double vol = soundOn() ? soundVolume() * 14.0 : 0.0;
    static double phase = 0.0; static int n0L = 0, n0R = 0, n1L = 0, n1R = 0;
    const double ratio = NATIVE_RATE / (double)devRate();
    if (vol <= 0.0) { for (int i = 0; i < frames * 2; ++i) out[i] = 0; return; }
    for (int f = 0; f < frames; ++f) {
        while (phase >= 1.0) { n0L = n1L; n0R = n1R; genNative(n1L, n1R); phase -= 1.0; }
        double fr = phase;
        int L = (int)((n0L * (1.0 - fr) + n1L * fr) * vol);
        int R = (int)((n0R * (1.0 - fr) + n1R * fr) * vol);
        if (L > 32767) L = 32767; if (L < -32768) L = -32768;
        if (R > 32767) R = 32767; if (R < -32768) R = -32768;
        out[2 * f] = (int16_t)L; out[2 * f + 1] = (int16_t)R;
        phase += ratio;
    }
}

// ── ВОСПРОИЗВЕДЕНИЕ ──────────────────────────────────────────────────────────────────────────────
// PCM-сэмпл по индексу (0..count-1) через DAC-канал. Берёт свободный DAC-голос (или вытесняет 0-й).
inline void play(int id) {
    if (!soundOn() || !dev() || id < 0 || id >= (int)samples().size()) return;
    SDL_LockAudioDevice(dev());
    DacVoice* best = nullptr; for (int i = 0; i < 6; ++i) if (!dacv()[i].active) { best = &dacv()[i]; break; }
    if (!best) best = &dacv()[0];
    best->s = &samples()[id]; best->pos = 0; best->step = best->s->rate / NATIVE_RATE; best->active = true;
    SDL_UnlockAudioDevice(dev());
}
// FM-патч как нота (синт-SFX). hold/tail в секундах.
inline void playFm(int patchIdx, int note = 60, double hold = 0.14, double tail = 0.45) {
    if (!soundOn() || !dev() || patchIdx < 0 || patchIdx >= (int)patches().size()) return;
    const FmPatch& fp = patches()[patchIdx]; if (!fp.ok) return;
    SDL_LockAudioDevice(dev());
    // выбрать голос: свободный, иначе самый старый (LRU)
    int pick = -1; for (int i = 0; i < FM_SFX_CH; ++i) if (!fmv()[i].active) { pick = i; break; }
    if (pick < 0) { long best = 1L << 62; for (int i = 0; i < FM_SFX_CH; ++i) if (fmv()[i].started < best) { best = fmv()[i].started; pick = i; } }
    FmVoice& v = fmv()[pick]; v.ch = pick;
    int part, cofs, keych; chAddr(v.ch, part, cofs, keych);
    if (v.active) wr(0, 0x28, keych);                         // снять прежнюю ноту голоса
    loadPatch(v.ch, fp);
    int block, fnum; noteToFnum(note, block, fnum);
    wr(part, 0xA4 + cofs, (block << 3) | (fnum >> 8));
    wr(part, 0xA0 + cofs, fnum & 0xFF);
    wr(0, 0x28, ((fp.key & 0xF) << 4) | keych);              // key-on (маска операторов из патча)
    long t = nativeClock();
    v.active = true; v.patch = patchIdx; v.started = t;
    v.keyOff = t + (long)(hold * NATIVE_RATE);
    v.freeAt = t + (long)((hold + tail) * NATIVE_RATE);
    SDL_UnlockAudioDevice(dev());
}
inline int patchCount() { return (int)patches().size(); }

// ── SFX-ДЕСКРИПТОРЫ ZT (таблица @0xc5eb4, 3 байта/запись: type,p1,p2 — дизасм sub_0c60c2/0xc615a) ──
// Игра триггерит звук по ID (d0): фон 0x1b=выстрел, 0x6b=смена оружия, 0x67=снаряд, 0x68=попадание…
//   type 0 = МУЗЫКА (секвенция p1=0..8) — отдельный проход (пока пропуск);
//   type 1 = FM-патч p1 + НОТА p2 (синт-SFX: смена/писк/двери/враги);
//   type 2/3 = FM-патч p1 на GEMS-канале 0xf (выстрелы оружия p1=48..53 — FM-патчи), нота по умолчанию.
struct SfxDesc { uint8_t type = 0, p1 = 0, p2 = 0; };
inline std::vector<SfxDesc>& sfxTable() { static std::vector<SfxDesc> v; return v; }
inline int sfxCount() { return (int)sfxTable().size(); }
// Сыграть SFX по ИГРОВОМУ id (как 0xc615a). Возвращает false если музыка/пусто (нечего играть сейчас).
// type 0 = музыка (секвенция p1=0..8) — отдельный проход;
// type 1 = FM-патч p1 + НОТА p2 (GEMS-канал 0xe: синт-SFX — двери/смена/снаряды/писк врага);
// type 2/3 = DAC-СЭМПЛ p1 (GEMS-канал 0xf: ПЕРКУССИЯ — выстрелы/взрывы/удары; p1=индекс сэмпла 0..72).
inline int& sfxTranspose() { static int t = 0; return t; }   // тюнинг высоты FM-SFX (GEMS-нота может требовать сдвига). Консоль `sfxtr`.
// МОДЕЛЬ (Z80-реверс GEMS @0x58F78 + подтверждения юзера): дескр. type определяет КЛАСС звука:
//   • type0 = МУЗЫКА (секвенция p1) — отдельный проход;
//   • type1 (cmd 0x02 @Z80 0x172a = «загрузить патч p1 и играть нотой p2») = FM-СИНТ (двери/смена/писк) — FM ЕСТЬ;
//        патч p1 почти всегда FM (исключения: DAC-патч → сэмпл нота−0x30; NOISE → скип);
//   • type2/3 = PCM/DAC-СЭМПЛ p1 (ВЫСТРЕЛЫ/ВЗРЫВЫ/удары — подтверждено юзером: это PCM, НЕ FM).
inline bool playSfx(int id) {
    if (id < 0 || id >= (int)sfxTable().size()) return false;
    const SfxDesc& d = sfxTable()[id];
    if (d.type == 0) return false;                         // музыка (секвенция) — GEMS-секвенсер, отдельно
    if (d.type == 1) {                                     // FM-СИНТ: загрузить патч p1, играть нотой p2
        if (d.p1 < (int)patches().size() && patches()[d.p1].ok) { playFm(d.p1, (d.p2 ? d.p2 : 60) + sfxTranspose()); return true; }
        if (d.p2 >= 0x30) { int s = d.p2 - 0x30; if (s < (int)samples().size()) { play(s); return true; } }  // DAC-патч: нота−0x30=сэмпл
        if (d.p1 < (int)samples().size()) { play(d.p1); return true; }
        return false;
    }
    if (d.p1 < (int)samples().size()) { play(d.p1); return true; }   // type2/3 = PCM/DAC-сэмпл p1 (выстрелы/взрывы)
    return false;
}

// ── ЗАГРУЗКА: таблица DAC-сэмплов + банк FM-патчей + инициализация чипа и аудио ──
inline void load(const Rom& rom, size_t sampBase = 0x5E51C, size_t patchBase = 0x5A804) {
    // 1) DAC-сэмплы (как ztextractor: подряд идущие записи 12 байт)
    samples().clear();
    size_t prevEnd = 0;
    for (int i = 0; i < 128; ++i) {
        size_t o = sampBase + (size_t)i * 12; if (o + 12 > rom.size()) break;
        uint32_t start = ((uint32_t)rom.u8(o + 3) << 16) | rom.u8(o + 1) | ((uint32_t)rom.u8(o + 2) << 8);
        int dlen = rom.u8(o + 6) | (rom.u8(o + 7) << 8);
        uint8_t flags = rom.u8(o);
        if (start == 0 && i > 0) break;
        size_t addr = sampBase + start;
        if (!(start >= 0x100 && start < 0x180000 && dlen >= 0x40 && dlen <= 0xFFFF && addr + dlen <= rom.size())) break;
        if (prevEnd && addr != prevEnd) break;
        Sample s; int div = 144 * (flags & 0x0F); s.rate = div ? (YM_CLOCK / div) : 10500.0;
        s.pcm.resize(dlen); for (int j = 0; j < dlen; ++j) s.pcm[j] = rom.u8(addr + j);
        samples().push_back(std::move(s)); prevEnd = addr + dlen;
    }
    // 2) FM-патчи (банк GEMS: [word: 2*count][word: off]… → запись: type+данные)
    patches().clear();
    uint32_t first = rom.u8(patchBase) | (rom.u8(patchBase + 1) << 8);
    int np = (int)(first / 2);
    if (np > 0 && np < 512) for (int i = 0; i < np; ++i) {
        uint32_t off = rom.u8(patchBase + i * 2) | (rom.u8(patchBase + i * 2 + 1) << 8);
        size_t a = patchBase + off;
        uint8_t buf[39] = {0};
        if (a + 39 <= rom.size()) for (int j = 0; j < 39; ++j) buf[j] = rom.u8(a + j);
        patches().push_back(decodeFmPatch(buf));            // не-FM → ok=false (пропустится при playFm)
    }
    // 2b) SFX-дескрипторы (таблица @0xc5eb4, 3 байта/запись) — авторитетная карта id→(type,patch,note)
    sfxTable().clear();
    for (int i = 0; i < 0xA0; ++i) { size_t o = 0xc5eb4 + (size_t)i * 3; if (o + 3 > rom.size()) break;
        SfxDesc d; d.type = rom.u8(o); d.p1 = rom.u8(o + 1); d.p2 = rom.u8(o + 2); sfxTable().push_back(d); }
    // 3) чип YM2612 + DAC включён (рег 0x2B бит7), DAC в центр
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip());
    inited() = true;
    // 4) SDL аудио (стерео S16 @44100); чип тактируется в колбэке
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    dev() = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev()) { devRate() = have.freq;
        wr(0, 0x2B, 0x80);                                   // DAC enable
        wr(0, 0x2A, 0x80);                                   // DAC центр
        SDL_PauseAudioDevice(dev(), 0);
    }
}

// ── СОБЫТИЯ → ИГРОВОЙ SFX-ID (выверено по дизасм-вызовам jsr 0xd740/0xd760 с d0=id) ────────────────
// Подтверждено: 0x1b=выстрел общий (0x12bac), 0x1a/0x1c=варианты оружия (0x1214e/0x12188), 0x6b=смена
// оружия+меню (0x1125e/0x582a4), 0x67=снаряд (0xb3f4 +враги), 0x68=попадание (d796 +враги), 0x8a=взрыв
// (0x15e8c урон-радиус), 0x6c=actor-звук (0x13b0a), 0x6d=дверь/коллизия (0x13ec8/0x143de).
enum Ev { SFX_HANDGUN, SFX_SHOTGUN, SFX_LASER, SFX_PULSE, SFX_ROCKET, SFX_GRENADE, SFX_EXPLOSION,
          SFX_FLAME, SFX_FOAM, SFX_PICKUP, SFX_HURT, SFX_DOOR, SFX_SWITCH, SFX_ENEMY_HIT,
          SFX_ENEMY_FIRE, SFX_ENEMY_DEATH, SFX_WALL, SFX_ELEVATOR, SFX_GRENADE_BOUNCE, SFX_MENU, SFX__COUNT };
inline int* evMap() { static int m[SFX__COUNT] = {
    /*HANDGUN  */ 0x1b, /*SHOTGUN */ 0x1a, /*LASER  */ 0x1c, /*PULSE  */ 0x1b, /*ROCKET */ 0x67,
    /*GRENADE  */ 0x67, /*EXPLOSION*/0x8a, /*FLAME  */ 0x1b, /*FOAM   */ 0x6d, /*PICKUP */ 0x6b,  // PICKUP=SWITCH (0x6b)!
    /*HURT     */ 0x68, /*DOOR    */ 0x6d, /*SWITCH */ 0x6b, /*ENEMY_HIT*/ 0x68,
    /*ENEMY_FIRE*/ 0x1b, /*ENEMY_DEATH*/ 0x2d, /*WALL*/ 0x3b, /*ELEVATOR*/ 0x6f, /*GRENADE_BOUNCE*/ 0x18, /*MENU*/ 0x6b }; return m; }
inline void ev(Ev e) { if (e < 0 || e >= SFX__COUNT) return; playSfx(evMap()[e]); }

}  // namespace snd
