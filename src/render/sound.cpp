// ztpp — src/render/sound.cpp: реализация звукового движка (YM2612/GEMS + SDL-воспроизведение).
// Тела «действий» вынесены из sound.hpp (состояние/синглтоны/структуры/SFX-карты — там, inline).
#include "sound.hpp"

namespace snd {

// Запись регистра YM2612 с тактированием (addr→24 такта→data→24 такта, иначе чип теряет запись).
void wr(int port, int addr, int data) {
    Bit16s b[2];
    OPN2_Write(&chip(), (Bit32u)(port ? 2 : 0),       (Bit8u)addr); for (int i = 0; i < 24; ++i) OPN2_Clock(&chip(), b);
    OPN2_Write(&chip(), (Bit32u)((port ? 2 : 0) | 1), (Bit8u)data); for (int i = 0; i < 24; ++i) OPN2_Clock(&chip(), b);
}

FmPatch decodeFmPatch(const uint8_t* p) {
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
// ⭐GEMS note→fnum ТОЧНО (Z80 $10aa + fnum_table $116a): block=note/12, fnum=T[note%12]. Нота 0..0x5F.
void gemsNoteToFnum(int note, int& block, int& fnum) {
    static const int T[12] = {0x284,0x2AA,0x2D3,0x2FE,0x32B,0x35B,0x38E,0x3C5,0x3FE,0x43B,0x47B,0x4BF};
    if (note < 0) note = 0; if (note > 0x5F) note = 0x5F;
    block = note / 12; if (block > 7) block = 7;
    fnum = T[note % 12];
}
void noteToFnum(double note, int& block, int& fnum) {
    double freq = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    block = 4;
    while (block < 7) { double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK; if (f < 0x800) break; ++block; }
    while (block > 0) { double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK; if (f >= 0x400) break; --block; }
    double f = freq * 144.0 * std::pow(2.0, 21 - block) / YM_CLOCK;
    fnum = (int)std::lround(f); if (fnum < 0) fnum = 0; if (fnum > 0x7FF) fnum = 0x7FF;
}

void loadPatch(int ch, const FmPatch& fp) {
    int part, cofs, keych; chAddr(ch, part, cofs, keych);
    for (int op = 0; op < 4; ++op) { int base = opOffset()[op] + cofs;
        for (int r = 0; r < 7; ++r) wr(part, opRegList()[r] + base, fp.op[op][r]); }
    wr(part, 0xB0 + cofs, fp.B0); wr(part, 0xB4 + cofs, fp.B4); wr(0, 0x22, fp.lfo);
}

// ── GEMS-СЕКВЕНСЕР (МУЗЫКА): порт mdgfx/gems_player.py (реалтайм, в аудио-потоке) ──
void gemsLoadPatchVoice(int v, int patchIdx) {
    if (patchIdx < 0 || patchIdx >= (int)patches().size() || !patches()[patchIdx].ok) return;
    loadPatch(v, patches()[patchIdx]);
}
void gemsKeyOffVoice(int v) {
    int part, cofs, keych; chAddr(v, part, cofs, keych); (void)part; (void)cofs;
    wr(0, 0x28, keych);
}
// ⭐ЖЁСТКОЕ ГЛУШЕНИЕ голоса (только СТОП/ПАУЗА): GEMS-патчи с Release Rate=0 НЕ глохнут по key-off —
// оператор висит на sustain вечно («залипшие ноты», юзер: музыка эп3 при паузе/победе над боссом,
// ноты висели на финальной заставке). SL=15/RR=15 всем операторам (0x80+op: D7-4=SL, D3-0=RR) →
// мгновенное затухание, затем key-off. Патч порчен → вызывающий сбрасывает кэш m.voice[].patch.
void gemsHardKillVoice(int v) {
    int part, cofs, keych; chAddr(v, part, cofs, keych);
    for (int op = 0; op < 4; ++op) wr(part, 0x80 + opOffset()[op] + cofs, 0xFF);
    wr(0, 0x28, keych);
}
void noiseOnRaw(int patchIdx);   // fwd (тело ниже, у playNoise) — нойз-перкуссия музыки из аудио-потока
void dacStartRaw(int s);         // fwd (тело ниже) — единственный DAC-голос, лочит FM-голос 5
long* chanReleased() { static long r[6] = {0,0,0,0,0,0}; return r; }   // такт освобождения канала (ROM +6 «возраст»)
// PSG ТОН (музыка): note-on — алло 3 голосов (свободный/чужой в release/кража), ADSR из psgInsts()[patch].
void psgMusNoteOn(int chIdx, int note, int patch, int chanVol) {
    PsgTone* T = psgTones();
    int v = -1;
    for (int i = 0; i < 3; ++i) if (T[i].owner == chIdx) { v = i; break; }               // свой голос
    if (v < 0) for (int i = 0; i < 3; ++i) if (!T[i].active) { v = i; break; }           // свободный
    if (v < 0) for (int i = 0; i < 3; ++i) if (T[i].phase >= 4 || T[i].phase == 0) { v = i; break; }   // в release
    if (v < 0) v = 0;                                                                     // кража
    PsgTone& t = T[v];
    t = PsgTone{}; t.active = true; t.owner = chIdx; t.phase = 1; t.att = 255;
    if (patch >= 0 && patch < (int)psgInsts().size()) {
        const PsgInst& pi = psgInsts()[patch];
        t.atk = pi.b[2] ? pi.b[2] : 255;
        t.sus = pi.b[3] << 4; t.tgt = pi.b[4] << 4;         // патч хранит 4-бит уровни → 8-бит внутр. (как noise)
        t.dec = pi.b[5] ? pi.b[5] : 1;   t.rel = pi.b[6] ? pi.b[6] : 8;
    }
    t.volAtt = (chanVol < 0 ? 0 : chanVol > 0x7F ? 0x7F : chanVol) >> 3;                  // аттенюация канала → атт4-сдвиг
    double freq = 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
    if (freq < 30) freq = 30;
    t.halfPeriod = NATIVE_RATE / (2.0 * freq);
}
void psgMusRelease(int v) { if (v >= 0 && v < 3 && psgTones()[v].active) { psgTones()[v].phase = 4; psgTones()[v].owner = -1; } }
// Освободить FM-голос музыки: key-off + владелец запоминается для own-reuse (ROM: запись хранит +3).
void gemsFreeVoice(GemsMusic& m, int v, bool keyoff) {
    if (m.voice[v].owner < 0) return;
    if (keyoff) gemsKeyOffVoice(v);
    voiceLastOwner()[v] = m.voice[v].owner;
    m.voice[v].owner = -1; chanPrio()[v] = 0; chanReleased()[v] = m.tick;
}
void gemsNoteOffCh(GemsMusic& m, GemsChan& c) {           // PSG-тон (FM гасится ПО ГОЛОСАМ в gemsTick)
    (void)m;
    if (c.psgV >= 0) { psgMusRelease(c.psgV); c.psgV = -1; c.note = -1; }
}
// ⭐GEMS-АЛЛОКАТОР (Z80 $17e2, сверен построчно 2026-07-17): общий для музыки и SFX. Возврат −1 = ОТКАЗ.
// Порядок сканирования записей ROM: phys 0,1,4,5,6,2 = порт-голоса 0,1,3,4,5,2 (тай-брейки решает ПЕРВЫЙ).
// (1) СВОБОДНЫЙ голос, чей последний владелец == запросивший, берётся сразу (own-reuse ПОСЛЕ релиза);
// (2) иначе свободный с min возрастом (+6: давнее всех освобождён — релиз отзвучал, срез неслышен);
// (3) свободных нет → кража у занятого с МИН. приоритетом, если reqPrio >= него; (4) иначе ОТКАЗ.
// Пока DAC играет PCM, голос 5 (phys6) исключён совсем (бит5 записи $17b1).
// ⭐ПРИОРИТЕТЫ РАВНЫ (68k c6106): рассылка таблицы приоритетов {ch13/14=1, ch15=2, ch12=3} ВЫРЕЗАНА из
// кода (jsr удалён, таблица c6146 мёртвая), Z80-RAM за драйвером занулён → ВСЕ каналы prio 0. Музыка
// крадёт у SFX и наоборот; отказов в ZT не бывает; жертву при равных решает порядок сканирования.
static const int MUS_PRIO = 1, SFX_PRIO = 1;
static const int VOICE_ORDER[6] = {0, 1, 3, 4, 5, 2};
int gemsAllocFm(int reqPrio, int ownerId) {
    GemsMusic& m = music();
    int best = -1; long bl = 0;
    for (int i = 0; i < 6; ++i) { int v = VOICE_ORDER[i];
        if (v == 5 && dacLocked()) continue;                            // бит5: голос за DAC
        bool busy = fmv()[v].active || m.voice[v].owner >= 0;
        if (busy) continue;
        if (voiceLastOwner()[v] == ownerId) return v;                   // (1) свой голос — сразу
        long l = chanReleased()[v];
        if (best < 0 || l < bl) { best = v; bl = l; }                   // (2) строгий <: тай-брейк первому
    }
    if (best >= 0) return best;
    int mn = -1, mp = 0x7F + 1;
    for (int i = 0; i < 6; ++i) { int v = VOICE_ORDER[i];
        if (v == 5 && dacLocked()) continue;
        if (chanPrio()[v] > 0 && chanPrio()[v] < mp) { mp = chanPrio()[v]; mn = v; } }
    if (mn < 0 || reqPrio < mp) return -1;                              // (4) ОТКАЗ (GEMS: звук не играет)
    // (3) кража: key-off у владельца (музыка или SFX)
    if (m.voice[mn].owner >= 0) { gemsFreeVoice(m, mn, true); m.voice[mn].patch = -1; m.voice[mn].vol = -1; }
    if (fmv()[mn].active) { int part, cofs, keych; chAddr(mn, part, cofs, keych); (void)part; (void)cofs;
        wr(0, 0x28, keych); fmv()[mn].active = false; voiceLastOwner()[mn] = -1; }
    return mn;
}
void gemsNoteOn(GemsMusic& m, int chIdx, int note) {
    GemsChan& c = m.ch[chIdx];
    int v = gemsAllocFm(MUS_PRIO, chIdx);
    if (v < 0) return;                                                  // отказ: нота музыки пропускается
    m.voice[v].owner = chIdx; m.voice[v].last = m.tick; chanPrio()[v] = MUS_PRIO; c.voice = v;
    int part, cofs, keych; chAddr(v, part, cofs, keych);
    const FmPatch* fp = (c.patch >= 0 && c.patch < (int)patches().size() && patches()[c.patch].ok) ? &patches()[c.patch] : nullptr;
    if (m.voice[v].patch != c.patch) { gemsLoadPatchVoice(v, c.patch); m.voice[v].patch = c.patch; m.voice[v].vol = -1; }
    if (fp && m.voice[v].vol != c.volume) {                            // ⭐громкость GEMS: c.volume = АТТЕНЮАЦИЯ (0=полная;
        uint8_t car = algCarriers()[fp->alg];                          //  данные 0x72/05 в треках = 0..40). Z80 15f3 мультипликатив:
        int att = c.volume; if (att < 0) att = 0; if (att > 0x7F) att = 0x7F;   //  loud_final = loud − (loud·2·att>>8); TL = 0x7F − loud_final
        for (int op = 0; op < 4; ++op) if (car & (1 << op)) {
            int loud = 0x7F - fp->op[op][1];
            int lf = loud - ((loud * 2 * att) >> 8); if (lf < 0) lf = 0;
            int tl = 0x7F - lf;
            if (tl > 0x7F) tl = 0x7F; if (tl < 0) tl = 0;
            wr(part, 0x40 + opOffset()[op] + cofs, tl);
        }
        m.voice[v].vol = c.volume;
    }
    int block, fnum; gemsNoteToFnum(note, block, fnum);                // ⭐ROM-таблица $116a (transpose не нужен)
    wr(part, 0xA4 + cofs, (block << 3) | (fnum >> 8));
    wr(part, 0xA0 + cofs, fnum & 0xFF);
    wr(0, 0x28, (((fp ? fp->key : 0xF) & 0xF) << 4) | keych);
    c.note = note;
}
void gemsDelayAdd(GemsChan& c) { c.wait = c.delayv; c.wasdelay = c.wasduration = false; }
// Обработать события канала до паузы (порт _advance).
void gemsAdvance(GemsMusic& m, int chIdx) {
    GemsChan& c = m.ch[chIdx];
    const std::vector<uint8_t>& d = m.seq;
    int guard = 0;
    while (!c.done && c.wait == 0 && guard++ < 10000) {
        if (c.pos < 0 || c.pos >= (int)d.size()) { c.done = true; return; }
        uint8_t b = d[c.pos];
        if (b >= 0xC0) { c.delayv   = c.wasdelay    ? c.delayv * 0x40 + (b - 0xC0)   : (b - 0xC0);   c.wasdelay = true;  c.wasduration = false; ++c.pos; continue; }
        if (b >= 0x80) { c.duration = c.wasduration ? c.duration * 0x40 + (b - 0x80) : (b - 0x80);   c.wasduration = true; c.wasdelay = false;  ++c.pos; continue; }
        if (b < 0x60) {                                                 // НОТА
            ++c.pos;
            if (c.patchType == 0) {                                     // FM: длительность НА ГОЛОСЕ (ROM +4/+5) —
                gemsNoteOn(m, chIdx, b);                                //  перекрывающиеся ноты канала звенят на РАЗНЫХ голосах
                if (c.voice >= 0 && m.voice[c.voice].owner == chIdx)
                    m.voice[c.voice].off = m.tick + (c.duration > 0 ? c.duration : 1);
            } else if (c.patchType == 1) {                              // DAC-ударные: сэмпл = нота − 0x30 (ROM $132e swap)
                int s = (int)b - 0x30; if (s < 0) s += 0x60;
                dacStartRaw(s);
            } else if (c.patchType == 2) {                              // ⭐PSG ТОН (Z80 psg_note_on_common $124c)
                psgMusNoteOn(chIdx, (int)b + musicTranspose(), c.patch, c.volume);
                for (int i = 0; i < 3; ++i) if (psgTones()[i].owner == chIdx) { c.psgV = i; break; }
                c.noteOff = m.tick + (c.duration > 0 ? c.duration : 1);
                c.note = b;
            } else if (c.patchType == 3) {                              // PSG NOISE (перкуссия музыки)
                noiseOnRaw(c.patch);
            }
            gemsDelayAdd(c);
            continue;
        }
        if (b == 0x60) { c.done = true; return; }                       // eos
        if (b == 0x65) {                                                // loopend
            ++c.pos;
            if (c.loopDepth > 0) {
                int& rem = c.loopRem[c.loopDepth - 1];
                if (rem == 0x7F)      c.pos = c.loopRet[c.loopDepth - 1];          // бесконечный луп
                else if (rem > 1)   { --rem; c.pos = c.loopRet[c.loopDepth - 1]; }
                else                  --c.loopDepth;
            }
            c.wasdelay = c.wasduration = false;
            continue;
        }
        int ln = 1;
        switch (b) { case 0x61: case 0x62: case 0x64: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6E: ln = 2; break;
                     case 0x6C: case 0x6F: case 0x70: case 0x72: ln = 3; break; case 0x71: ln = 5; break; default: ln = 1; }
        uint8_t arg = (ln >= 2 && c.pos + 1 < (int)d.size()) ? d[c.pos + 1] : 0;
        if (b == 0x61) {                                                // patch
            c.patch = arg;
            c.patchType = (arg < patchTypes().size()) ? patchTypes()[arg] : -1;
            c.pos += ln; gemsDelayAdd(c); continue;
        }
        if (b == 0x64) {                                                // loop N (0x7F=∞)
            c.pos += ln;
            if (c.loopDepth < 8) { c.loopRet[c.loopDepth] = c.pos; c.loopRem[c.loopDepth] = arg; ++c.loopDepth; }
            c.wasdelay = c.wasduration = false; continue;
        }
        if (b == 0x68) { m.tempo = arg + 40; c.pos += ln; gemsDelayAdd(c); continue; }   // tempo
        if (b == 0x6D) { c.pos += 1; gemsDelayAdd(c); continue; }   // ⭐Z80 070c: SET3 флага канала, темп НЕ меняет (был «tempo=150» — неверно)
        if (b == 0x6B) {                                                // play: DAC-сэмпл arg напрямую
            dacStartRaw((int)arg);
            c.pos += ln; gemsDelayAdd(c); continue;
        }
        if (b == 0x6F) {                                                // goto (word LE — офсет в банке)
            if (c.pos + 2 < (int)d.size()) c.pos = d[c.pos + 1] | (d[c.pos + 2] << 8); else c.done = true;
            c.wasdelay = c.wasduration = false; continue;
        }
        if (b == 0x72) {                                                // extra: sub5 = громкость канала
            if (c.pos + 2 < (int)d.size() && d[c.pos + 1] == 5) c.volume = d[c.pos + 2];
            c.pos += ln; gemsDelayAdd(c); continue;
        }
        c.pos += ln; gemsDelayAdd(c);                                    // прочее — скип
    }
}
// Один GEMS-тик музыки (вызывается из genNative по темпу).
void gemsTick(GemsMusic& m) {
    for (int i = 0; i < m.nch; ++i) if (!m.ch[i].done) gemsAdvance(m, i);
    // key-off ПО ГОЛОСАМ (ROM 0806/0828: длительность на записи голоса, канал может уже играть следующую ноту)
    for (int v = 0; v < 6; ++v) if (m.voice[v].owner >= 0 && m.tick >= m.voice[v].off) gemsFreeVoice(m, v, true);
    for (int i = 0; i < m.nch; ++i) { GemsChan& c = m.ch[i];
        if (c.noteOff >= 0 && m.tick >= c.noteOff) { gemsNoteOffCh(m, c); c.noteOff = -1; } }   // PSG
    for (int i = 0; i < m.nch; ++i) if (m.ch[i].wait > 0) --m.ch[i].wait;
    ++m.tick;
}
// Стоп: снять все ноты музыки (вызывать под LockAudioDevice).
void musicStopLocked() {
    for (int i = 0; i < 3; ++i) { psgTones()[i].active = false; psgTones()[i].owner = -1; }
    GemsMusic& m = music();
    for (int v = 0; v < 6; ++v) { gemsFreeVoice(m, v, false); voiceLastOwner()[v] = -1;
        if (!fmv()[v].active) gemsHardKillVoice(v);   // ⭐HARD-KILL: key-off не глушит патчи с RR=0 (залипание)
        m.voice[v].patch = -1; m.voice[v].vol = -1;   //   патч порчен (RR=15) → форс перезагрузки при след. ноте
    }
    for (int i = 0; i < m.nch; ++i) gemsNoteOffCh(m, m.ch[i]);
    m.active = false; m.paused = false; m.song = -1;
}

// ── ГЕНЕРАЦИЯ ОДНОГО НАТИВНОГО СЭМПЛА (аудио-поток) ──
void genNative(int& outL, int& outR) {
    long t = ++nativeClock();
    // МУЗЫКА: GEMS-тик по темпу (1 тик = 2.5/tempo сек → NATIVE_RATE*2.5/tempo сэмплов)
    { GemsMusic& m = music();
      if (m.active && !m.paused) {                            // ⭐пауза (cmd 0x0D): секвенсер заморожен
          m.acc -= 1.0;
          if (m.acc <= 0.0) { gemsTick(m); m.acc += NATIVE_RATE * 2.5 / (m.tempo < 1 ? 1 : m.tempo); }
      } }
    // FM SFX: расписание key-off / освобождение голоса
    for (int i = 0; i < FM_SFX_CH; ++i) { FmVoice& v = fmv()[i]; if (!v.active) continue;
        int part, cofs, keych; chAddr(v.ch, part, cofs, keych);
        if (v.keyOff && t >= v.keyOff) { wr(0, 0x28, keych); v.keyOff = 0; }   // key-off (маска операторов 0)
        if (t >= v.freeAt) { v.active = false; chanPrio()[v.ch] = 0; chanReleased()[v.ch] = music().tick; }
    }
    // DAC PCM: ОДИН голос (ROM: GEMS имеет единственный DAC-канал, новый сэмпл крадёт его)
    int dacActive = 0, dacByte = 0x80;                        // центр = тишина
    { DacVoice& d = dacv()[0];
      if (d.active) {
          size_t idx = (size_t)d.pos;
          if (idx >= d.s->pcm.size()) d.active = false;
          else { dacByte = d.s->pcm[idx]; d.pos += d.step; dacActive = 1; }
      } }
    // ⭐$2B ДИНАМИЧЕСКИ (ROM): DAC включён ТОЛЬКО пока PCM играет — иначе FM ch6 (голос 5) глушится!
    if (dacActive && !dacOn())  { wr(0, 0x2B, 0x80); dacOn() = true; }
    if (!dacActive && dacOn())  { wr(0, 0x2B, 0x00); dacOn() = false;
        if (dacLocked()) { dacLocked() = false; chanReleased()[5] = music().tick; } }   // снять бит5-лок голоса 5
    // 24 такта чипа = один сэмпл, с впис. DAC-записью (addr/data разнесены тактами для консумации FIFO)
    Bit16s b[2]; long l = 0, r = 0; int s = 0;
    OPN2_Write(&chip(), 0, 0x2A);                             // DAC addr
    for (; s < 12; ++s) { OPN2_Clock(&chip(), b); l += b[0]; r += b[1]; }
    OPN2_Write(&chip(), 1, (Bit8u)dacByte);                  // DAC data
    for (; s < 24; ++s) { OPN2_Clock(&chip(), b); l += b[0]; r += b[1]; }
    // ⭐PSG ТОНА МУЗЫКИ (SN76489 ch0-2 + GEMS ADSR psg_service): меандр, огибающая на noiseEnvHz (60 Гц)
    for (int tv = 0; tv < 3; ++tv) { PsgTone& t = psgTones()[tv];
        if (!t.active) continue;
        t.envAcc += 1.0;
        double envStep = NATIVE_RATE / (noiseEnvHz() > 1.0 ? noiseEnvHz() : 1.0);
        while (t.envAcc >= envStep) {
            t.envAcc -= envStep;
            switch (t.phase) {
                case 1: t.att -= t.atk; if (t.att <= t.tgt) { t.att = t.tgt; t.phase = 2; } break;      // ATTACK
                case 2: if (t.att < t.sus) { t.att += t.dec; if (t.att >= t.sus) { t.att = t.sus; t.phase = 3; } }
                        else if (t.att > t.sus) { t.att -= t.dec; if (t.att <= t.sus) { t.att = t.sus; t.phase = 3; } }
                        else t.phase = 3; break;                                                        // DECAY
                case 3: break;                                                                          // SUSTAIN
                case 4: t.att += t.rel; if (t.att >= 255) { t.att = 255; t.phase = 0; t.active = false; } break;  // RELEASE
                default: break;
            }
            if (t.att < 0) t.att = 0; if (t.att > 255) t.att = 255;
        }
        if (!t.active) continue;
        t.acc += 1.0;
        while (t.acc >= t.halfPeriod) { t.acc -= t.halfPeriod; t.cur = -t.cur; }
        int a4 = ((int)t.att >> 4) + t.volAtt; if (a4 > 15) a4 = 15;
        long amp = (long)(t.cur * noisePeak() * psgAmp(a4));
        l += amp; r += amp;
    }
    // PSG NOISE (SN76489 + GEMS ADSR): настоящий LFSR-шум, программная огибающая ведёт 4-бит аттенюацию
    { NoiseVoice& n = noiseV();
      if (n.active) {
          n.envAcc += 1.0;
          double envStep = NATIVE_RATE / (noiseEnvHz() > 1.0 ? noiseEnvHz() : 1.0);
          while (n.envAcc >= envStep) {
              n.envAcc -= envStep;
              switch (n.phase) {
                  case 1: n.att -= n.atk; if (n.att <= n.tgt) { n.att = n.tgt; n.phase = 2; } break;   // ATTACK
                  case 2: if (n.att < n.sus) { n.att += n.dec; if (n.att >= n.sus) { n.att = n.sus; n.phase = 3; } }
                          else if (n.att > n.sus) { n.att -= n.dec; if (n.att <= n.sus) { n.att = n.sus; n.phase = 3; } }
                          else n.phase = 3; break;                                                     // DECAY -> SUSTAIN
                  case 3: break;                                                                       // SUSTAIN (держит)
                  case 4: n.att += n.rel; if (n.att >= 255) { n.att = 255; n.phase = 0; } break;       // RELEASE -> idle
                  default: break;
              }
              if (n.att < 0) n.att = 0; if (n.att > 255) n.att = 255;
              if ((n.att >> 4) >= 15 && n.phase >= 2) n.active = false;   // перкуссия отзвучала → голос свободен
          }
          if (n.active) {
              n.shiftAcc += 1.0;
              while (n.shiftAcc >= n.shiftPeriod) { n.shiftAcc -= n.shiftPeriod;
                  int b0 = n.lfsr & 1, b3 = (n.lfsr >> 3) & 1;
                  uint16_t fb = (uint16_t)(n.white ? (b0 ^ b3) : b0);       // белый: бит0^бит3 (Sega) / период.: бит0
                  n.lfsr = (uint16_t)((n.lfsr >> 1) | (fb << 15));
                  n.cur = (n.lfsr & 1) ? 1 : -1;
              }
              long amp = (long)(n.cur * noisePeak() * psgAmp(n.att >> 4));
              l += amp; r += amp;
          }
          if (++n.life > (long)(NATIVE_RATE * 2)) n.active = false;         // предохранитель 2 с
      } }
    if (l > 32767) l = 32767; if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; if (r < -32768) r = -32768;
    outL = (int)l; outR = (int)r;
}

// SDL callback: ресемпл NATIVE→devRate (линейный) + громкость/мьют.
void mix(void*, Uint8* stream, int len) {
    int16_t* out = (int16_t*)stream;
    int frames = len / (int)(2 * sizeof(int16_t));            // стерео S16
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

// ── ВОСПРОИЗВЕДЕНИЕ ──
void play(int id) {
    if (!soundOn() || !dev() || id < 0 || id >= (int)samples().size()) return;
    SDL_LockAudioDevice(dev());
    dacStartRaw(id);
    SDL_UnlockAudioDevice(dev());
}
// ROM dac_note_on ($12f9): единственный DAC-канал GEMS — новый PCM КРАДЁТ играющий; на время PCM
// FM-голос 5 (phys6) отбирается у музыки/SFX и лочится (бит5 записи $17b1) — DAC глушит FM ch6.
void dacStartRaw(int s) {
    if (s < 0 || s >= (int)samples().size() || samples()[s].pcm.empty()) return;
    GemsMusic& m = music();
    if (!dacLocked()) {
        if (m.voice[5].owner >= 0) { gemsFreeVoice(m, 5, true); m.voice[5].patch = -1; m.voice[5].vol = -1; }
        if (fmv()[5].active) { int part, cofs, keych; chAddr(5, part, cofs, keych); (void)part; (void)cofs;
            wr(0, 0x28, keych); fmv()[5].active = false; voiceLastOwner()[5] = -1; }
        dacLocked() = true;
    }
    DacVoice& d = dacv()[0];
    d.s = &samples()[s]; d.pos = 0; d.step = d.s->rate / NATIVE_RATE; d.active = true;
}
// ЯДРО без лока (зов из аудио-потока секвенсером ИЛИ под локом из playNoise)
void playNoise(int patchIdx) {
    if (!soundOn() || !dev()) return;
    SDL_LockAudioDevice(dev());
    noiseOnRaw(patchIdx);
    SDL_UnlockAudioDevice(dev());
}
void noiseOnRaw(int patchIdx) {
    NoiseVoice& n = noiseV();
    n = NoiseVoice{};                                        // сброс к дефолтам (atk/dec/sus/rel = «выстрел»)
    n.active = true; n.lfsr = 0x8000; n.phase = 1; n.att = 255;
    uint8_t ctrl = 0x06;                                     // дефолт: белый шум, NF=2
    if (patchIdx >= 0 && patchIdx < (int)psgInsts().size()) {
        const PsgInst& pi = psgInsts()[patchIdx];
        ctrl  = pi.b[1];
        if (pi.b[2]) n.atk = pi.b[2];
        n.sus = pi.b[3] << 4;
        n.tgt = pi.b[4] << 4;
        if (pi.b[5]) n.dec = pi.b[5];
        if (pi.b[6]) n.rel = pi.b[6];
    }
    n.white = ((ctrl >> 2) & 1) != 0;                        // FB бит2 (1=белый / 0=периодический)
    // SN76489: NF=0/1/2 → LFSR-клок = мастер(3.58МГц)/{512,1024,2048}; NF=3 = tone2-driven (фолбэк).
    // ⭐ВЕРИФИЦИРОВАНО захватом: гуншот NF=2 → /2048 = 1748 Гц.
    double shiftHz;
    if (noiseRateOv() > 0)      shiftHz = NATIVE_RATE / (double)noiseRateOv();
    else { int nf = ctrl & 3;   double divider = (nf < 3) ? (512.0 * (1 << nf)) : 1024.0;  // nf=3 tone2-driven: фолбэк
           shiftHz = 3579545.0 / divider; }
    n.shiftPeriod = NATIVE_RATE / shiftHz; if (n.shiftPeriod < 1.0) n.shiftPeriod = 1.0;
}
void playFm(int patchIdx, int note, double hold, double tail) {
    if (!soundOn() || !dev() || patchIdx < 0 || patchIdx >= (int)patches().size()) return;
    const FmPatch& fp = patches()[patchIdx]; if (!fp.ok) return;
    SDL_LockAudioDevice(dev());
    // ⭐АЛЬТЕРНАТОР SFX (68k c61c6/c6220): FM-SFX идут на ДВА выделенных GEMS-канала 0xD/0xE попеременно
    // ($FF270A), с note-off ПРЕДЫДУЩЕЙ ноты канала перед новой (cmd1) — хвост звенит до следующего-через-
    // один SFX. Каналы зарезервированы от музыки (cmd 0x1C бит5), но ГОЛОСА — общий пул с равным prio.
    static int alt = 0; static int slotVoice[2] = {-1, -1};
    alt ^= 1; int slotOwner = 13 + alt;                       // владельцы = GEMS-каналы 13/14
    if (slotVoice[alt] >= 0) { FmVoice& pv = fmv()[slotVoice[alt]];
        if (pv.active && voiceLastOwner()[slotVoice[alt]] == slotOwner) {   // note-off прежней ноты слота
            int pp, pc, pk; chAddr(pv.ch, pp, pc, pk); (void)pp; (void)pc;
            wr(0, 0x28, pk); pv.active = false; pv.keyOff = 0;
            chanReleased()[pv.ch] = music().tick;
        }
        slotVoice[alt] = -1; }
    int pick = gemsAllocFm(SFX_PRIO, slotOwner);              // own-reuse: слот липнет к своему голосу
    if (pick < 0) { SDL_UnlockAudioDevice(dev()); return; }   // отказ (в ZT при равных prio недостижим)
    FmVoice& v = fmv()[pick]; v.ch = pick;
    int part, cofs, keych; chAddr(v.ch, part, cofs, keych);
    chanPrio()[pick] = SFX_PRIO; voiceLastOwner()[pick] = slotOwner; slotVoice[alt] = pick;
    music().voice[pick].patch = -1; music().voice[pick].vol = -1;   // инвалидировать патч-кеш музыки (канал перегружен)
    loadPatch(v.ch, fp);
    int block, fnum; noteToFnum(note, block, fnum);
    wr(part, 0xA4 + cofs, (block << 3) | (fnum >> 8));
    wr(part, 0xA0 + cofs, fnum & 0xFF);
    wr(0, 0x28, ((fp.key & 0xF) << 4) | keych);              // key-on (маска операторов из патча)
    long t = nativeClock();
    v.active = true; v.patch = patchIdx; v.started = t;
    v.keyOff = t + (long)(hold * NATIVE_RATE);
    v.freeAt = v.keyOff;                                      // ⭐канал СВОБОДЕН с key-off (ROM: релиз дозванивает,
                                                              // канал доступен; hold+tail блокировал → музыка глохла)
    SDL_UnlockAudioDevice(dev());
}

// ── МУЗЫКА ──
void musicStop() { if (!dev()) return; SDL_LockAudioDevice(dev()); musicStopLocked(); SDL_UnlockAudioDevice(dev()); }
// ⭐ПАУЗА (ROM GEMS cmd 0x0D sustain-all / 0x0C resume-all, 68k-обёртки c6344/c633a: пауза игры):
// секвенции замирают (SET4), звучащие ноты глушатся, позиции/тик сохранены — resume продолжает с места.
void musicSetPaused(bool p) {
    if (!dev()) return;
    SDL_LockAudioDevice(dev());
    GemsMusic& m = music();
    if (m.active && m.paused != p) {
        m.paused = p;
        if (p) {
            // ⭐ЖЁСТКИЙ key-off ВСЕХ голосов (как musicStopLocked): gemsFreeVoice выходит рано при owner<0,
            // поэтому голос с потерянным/украденным owner дронил бы на паузе. Страховка: key-off всех FM-голосов,
            // не занятых активным SFX, + принудительный сброс PSG-тонов (release оставлял «хвост» звучать).
            for (int v = 0; v < 6; ++v) { gemsFreeVoice(m, v, false);
                if (!fmv()[v].active) gemsHardKillVoice(v);       // ⭐HARD-KILL (RR=0-патчи не глохнут key-off'ом)
                m.voice[v].patch = -1; m.voice[v].vol = -1; }     //   форс перезагрузки патча на resume
            for (int i = 0; i < m.nch; ++i) gemsNoteOffCh(m, m.ch[i]);
            for (int i = 0; i < 3; ++i) { psgTones()[i].active = false; psgTones()[i].owner = -1; }
        }
    }
    SDL_UnlockAudioDevice(dev());
}
bool musicPlay(int song, int tempo) {
    GemsMusic& m = music();
    if (!dev() || m.seq.empty()) return false;
    int n = musicSongCount();
    if (song < 0 || song >= n) return false;
    SDL_LockAudioDevice(dev());
    musicStopLocked();
    int st = m.seq[song * 2] | (m.seq[song * 2 + 1] << 8);
    if (st + 1 >= (int)m.seq.size()) { SDL_UnlockAudioDevice(dev()); return false; }
    int nch = m.seq[st];
    if (nch < 1 || nch > GEMS_MAX_CH) { SDL_UnlockAudioDevice(dev()); return false; }
    m.nch = nch;
    int stride = m.ptr3 ? 3 : 2;                    // ⭐ZT: 2-байтовые ptr; ZTU (GEMS-флаг «3»): 3-байтовые
    for (int c = 0; c < nch; ++c) {
        m.ch[c] = GemsChan{};
        size_t a = (size_t)st + 1 + (size_t)c * stride;
        m.ch[c].pos = m.seq[a] | (m.seq[a + 1] << 8) | (m.ptr3 ? ((int)m.seq[a + 2] << 16) : 0);
        m.ch[c].done = false;
    }
    for (int v = 0; v < 6; ++v) m.voice[v] = GemsVoice{};
    // ⭐ТЕМП ИЗ SFX-ТАБЛИЦЫ (ROM: 68k шлёт cmd5→0e1d с p2 записи ПЕРЕД стартом каждого трека:
    // (08be)=p2·436>>8 — та же формула, что seq-cmd 0x68; game over=82, лифт=120, эпизоды=128/134/128,
    // брифинг/титул=124, победа=128). «Наследование» было неверно — темп трека задаёт таблица.
    if (tempo > 0) m.tempo = tempo;
    else if (m.tempo < 40) m.tempo = 120;
    m.acc = 0; m.tick = 0; m.song = song; m.active = true;
    SDL_UnlockAudioDevice(dev());
    return true;
}

// ── ОТЛАДКА: headless-рендер трека в WAV (без аудио-устройства) — проверка секвенсера/PSG ──
bool renderMusicWav(int song, double seconds, const char* path) {
    GemsMusic& m = music();
    if (m.seq.empty()) return false;
    int n = musicSongCount(); if (song < 0 || song >= n) return false;
    musicStopLocked();
    int st = m.seq[song * 2] | (m.seq[song * 2 + 1] << 8);
    if (st + 1 >= (int)m.seq.size()) return false;
    int nch = m.seq[st]; if (nch < 1 || nch > GEMS_MAX_CH) return false;
    m.nch = nch;
    for (int c = 0; c < nch; ++c) { m.ch[c] = GemsChan{}; m.ch[c].pos = m.seq[st + 1 + c * 2] | (m.seq[st + 2 + c * 2] << 8); m.ch[c].done = false; }
    for (int v = 0; v < 6; ++v) m.voice[v] = GemsVoice{};
    if (m.tempo < 40) m.tempo = 120;
    m.acc = 0; m.tick = 0; m.song = song; m.active = true;
    long frames = (long)(seconds * NATIVE_RATE);
    std::vector<int16_t> pcm; pcm.reserve(frames);
    long psgOn = 0;
    for (long i = 0; i < frames; ++i) {
        int L, R; genNative(L, R);
        int v = L * 8; if (v > 32767) v = 32767; if (v < -32768) v = -32768;
        pcm.push_back((int16_t)v);
        if (i % 4410 == 0) for (int t = 0; t < 3; ++t) if (psgTones()[t].active) ++psgOn;
    }
    m.active = false;
    FILE* f = std::fopen(path, "wb"); if (!f) return false;
    uint32_t dlen = (uint32_t)(pcm.size() * 2), rate = (uint32_t)NATIVE_RATE;
    uint32_t riff = 36 + dlen; uint16_t fmt16 = 1, ch1 = 1, bits = 16; uint32_t brate = rate * 2; uint16_t balign = 2;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fsz = 16; std::fwrite(&fsz, 4, 1, f); std::fwrite(&fmt16, 2, 1, f); std::fwrite(&ch1, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&brate, 4, 1, f); std::fwrite(&balign, 2, 1, f); std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f); std::fwrite(&dlen, 4, 1, f); std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "music %d: %.1fs → %s (psg-active samples: %ld)\n", song, seconds, path, psgOn);
    return true;
}

// ── SFX по игровому id ──
bool playSfx(int id) {
    if (id < 0 || id >= (int)sfxTable().size()) return false;
    const SfxDesc& d = sfxTable()[id];
    if (d.type == 0) {                                     // МУЗЫКА: секвенция p1, ТЕМП p2 (cmd5/0e1d)
        if (!musicEnabled()) return false;
        return musicPlay(d.p1, d.p2);
    }
    if (d.type == 1) {                                     // FM-СИНТ: загрузить патч p1, играть нотой p2
        if (d.p1 < (int)patches().size() && patches()[d.p1].ok) { playFm(d.p1, d.p2 + sfxTranspose(), sfxHold(), sfxTail()); return true; }  // нота = p2 (для 0x68 гуншот p2=0)
        if (d.p1 < (int)patchTypes().size() && patchTypes()[d.p1] == 3) { playNoise(d.p1); return true; }  // NOISE (PSG SN76489)
        if (d.p2 >= 0x30) { int s = d.p2 - 0x30; if (s < (int)samples().size()) { play(s); return true; } }  // DAC-патч: нота−0x30=сэмпл
        if (d.p1 < (int)samples().size()) { play(d.p1); return true; }
        return false;
    }
    { int s = dacSampleIdx(d); if (s >= 0 && s < (int)samples().size()) { play(s); return true; } }   // type2/3 DAC (type2→−48 ремап)
    return false;
}

// ЛИФТ: 0x6f ОДНОЙ ДЛИННОЙ нотой (гудение на всю поездку). Патч 0x1a имеет долгую volume-огибающую.
void playElevatorHum() {
    if (0x6f >= (int)sfxTable().size() || !dev()) return;
    const SfxDesc& d = sfxTable()[0x6f];
    if (d.type == 1 && d.p1 < (int)patches().size() && patches()[d.p1].ok)
        playFm(d.p1, d.p2 + sfxTranspose(), 3.0, 0.8);        // hold 3с — долгое гудение одной нотой
}

// SOUND TEST: играть звук по id с ЯВНОЙ нотой (только type1 FM); type2/3 → DAC-сэмпл p1; type0 — скип.
void playSfxNote(int id, int note) {
    if (id < 0 || id >= (int)sfxTable().size() || !dev()) return;
    const SfxDesc& d = sfxTable()[id];
    if (d.type == 0) return;                               // музыка — в тесте не запускаем
    if (d.type == 1) {                                     // FM-синт: патч p1, ЯВНАЯ нота
        if (d.p1 < (int)patches().size() && patches()[d.p1].ok) { playFm(d.p1, note, sfxHold(), sfxTail()); return; }
        if (d.p1 < (int)patchTypes().size() && patchTypes()[d.p1] == 3) { playNoise(d.p1); return; }  // NOISE (PSG)
        if (d.p1 < (int)samples().size()) play(d.p1);      // DAC-патч — сэмпл
        return;
    }
    { int s = dacSampleIdx(d); if (s >= 0 && s < (int)samples().size()) play(s); }   // type2/3 → DAC-сэмпл
}
// SOUND TEST: заглушить все SFX-голоса (FM key-off + DAC-стоп). Музыку не трогаем.
void stopAllSfx() {
    if (!dev()) return;
    SDL_LockAudioDevice(dev());
    for (int i = 0; i < FM_SFX_CH; ++i) { FmVoice& v = fmv()[i]; if (v.active) {
        int part, cofs, keych; chAddr(v.ch, part, cofs, keych); (void)part; (void)cofs;
        wr(0, 0x28, keych); v.active = false; v.keyOff = 0; } }
    for (int i = 0; i < 6; ++i) dacv()[i].active = false;
    noiseV().active = false;
    SDL_UnlockAudioDevice(dev());
}

// ── ЗАГРУЗКА: таблица DAC-сэмплов + банк FM-патчей + секвенции + SFX-таблица + чип/аудио ──
void load(const Rom& rom, size_t sampBase, size_t patchBase, size_t seqBase, size_t sfxBase) {
    // 1) DAC-сэмплы (записи 12 байт). Пустые слоты-заглушки (start=0,dlen=0) ЗАНИМАЮТ индекс (тишина),
    //    но НЕ обрывают таблицу. ZT: пустые на 73 и 75; всего 93 слота (91 сэмпл).
    samples().clear();
    size_t prevEnd = 0;
    for (int i = 0; i < 128; ++i) {
        size_t o = sampBase + (size_t)i * 12; if (o + 12 > rom.size()) break;
        uint32_t start = ((uint32_t)rom.u8(o + 3) << 16) | rom.u8(o + 1) | ((uint32_t)rom.u8(o + 2) << 8);
        int dlen = rom.u8(o + 6) | (rom.u8(o + 7) << 8);
        uint8_t flags = rom.u8(o);
        if (start == 0 && dlen == 0) { samples().push_back(Sample{}); continue; }   // пустой слот: тишина, индекс сохранён
        size_t addr = sampBase + start;
        if (!(start >= 0x100 && start < 0x180000 && dlen >= 0x40 && dlen <= 0xFFFF && addr + dlen <= rom.size())) break;
        if (prevEnd && addr != prevEnd) break;
        Sample s; int div = 144 * (flags & 0x0F); s.rate = div ? (YM_CLOCK / div) : 10500.0;
        s.pcm.resize(dlen); for (int j = 0; j < dlen; ++j) s.pcm[j] = rom.u8(addr + j);
        samples().push_back(std::move(s)); prevEnd = addr + dlen;
    }
    // 2) FM-патчи (банк GEMS: [word: 2*count][word: off]… → запись: type+данные)
    patches().clear(); patchTypes().clear(); psgInsts().clear();
    uint32_t first = rom.u8(patchBase) | (rom.u8(patchBase + 1) << 8);
    int np = (int)(first / 2);
    if (np > 0 && np < 512) for (int i = 0; i < np; ++i) {
        uint32_t off = rom.u8(patchBase + i * 2) | (rom.u8(patchBase + i * 2 + 1) << 8);
        size_t a = patchBase + off;
        uint8_t buf[39] = {0};
        if (a + 39 <= rom.size()) for (int j = 0; j < 39; ++j) buf[j] = rom.u8(a + j);
        patches().push_back(decodeFmPatch(buf));            // не-FM → ok=false (пропустится при playFm)
        patchTypes().push_back(buf[0]);                     // тип: 0 FM / 1 DAC / 2 PSG / 3 NOISE
        PsgInst pi; for (int j = 0; j < 7; ++j) pi.b[j] = buf[j]; psgInsts().push_back(pi);  // сырые байты для PSG/NOISE ADSR
    }
    // 2c) банк СЕКВЕНЦИЙ (музыка): окно 64К от seqBase
    { // seqBase per-build (ZT 0x5B0CC — find_gems_banks; ZTU 0x2A288 — GEMS-init @0x96492)
      music().seq.clear();
      if (seqBase < rom.size()) {
          size_t n = rom.size() - seqBase; if (n > 0x10000) n = 0x10000;
          music().seq.resize(n);
          for (size_t j = 0; j < n; ++j) music().seq[j] = rom.u8(seqBase + j);
      }
      // ⭐ШИРИНА ptr КАНАЛОВ ПЕСНИ: 2 байта (ZT/BZT) или 3 (GEMS-флаг «3»: ZTU-драйвер — другая
      // сборка GEMS, диффом Z80-блоба ZT $58F78 vs ZTU 0x27FBC совпадает лишь 22%). Авто-детект =
      // mdgfx/gems_music.py::detect_ptr3: верная ширина даёт МОНОТОННО растущие ptr внутри песни.
      { const std::vector<uint8_t>& q = music().seq;
        auto u16 = [&](size_t a) { return (int)q[a] | ((int)q[a + 1] << 8); };
        int nsongs = q.size() > 2 ? u16(0) / 2 : 0;
        auto bad = [&](int stride) {
            int score = 0;
            for (int s = 0; s < nsongs && s < 6; ++s) {
                size_t st = (size_t)u16((size_t)s * 2);
                if (st + 1 >= q.size()) { score += 5; continue; }
                int nc = q[st];
                if (nc < 1 || nc > 16) { score += 5; continue; }
                long prev = -1;
                for (int c = 0; c < nc; ++c) {
                    size_t a = st + 1 + (size_t)c * stride;
                    if (a + stride > q.size()) { score += 5; break; }
                    long p = u16(a) | (stride == 3 ? ((long)q[a + 2] << 16) : 0);
                    if (p < prev) ++score;
                    prev = p;
                }
            }
            return score;
        };
        music().ptr3 = nsongs > 0 && bad(3) < bad(2);
      } }
    // 2b) SFX-дескрипторы (таблица per-build, 3 байта/запись) — авторитетная карта id→(type,patch,note)
    sfxTable().clear();
    for (int i = 0; i < 0xA2; ++i) { size_t o = sfxBase + (size_t)i * 3; if (o + 3 > rom.size()) break;   // 486 байт = 162 записи (0xa0 «secured»/0xa1 обрезались!)
        SfxDesc d; d.type = rom.u8(o); d.p1 = rom.u8(o + 1); d.p2 = rom.u8(o + 2); sfxTable().push_back(d); }
    // 3) чип YM2612 + DAC включён
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&chip());
    inited() = true;
    // 4) SDL аудио (стерео S16 @44100); чип тактируется в колбэке
    SDL_AudioSpec want{}, have{};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024; want.callback = mix;
    dev() = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev()) { devRate() = have.freq;
        wr(0, 0x2A, 0x80);                                   // DAC центр ($2B ДИНАМИЧЕСКИ в genNative:
        SDL_PauseAudioDevice(dev(), 0);                      //  вкл. на init глушил FM ch6 до первого PCM)
    }
}

}  // namespace snd
