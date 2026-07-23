// ztpp — src/password.hpp: система ПАРОЛЕЙ Zero Tolerance, бит-в-бит по ROM (2026-07-17).
// Генератор 58720 / валидатор 58a74 / бит-упаковка 58660/5868a / шифр 586b4/586e8 (перестановка
// 54 бит @0x5862a + XOR-поток ключа 0x56CA2D69 ror7+swap) / алфавит 64 симв @0x585ea /
// декод символа 58f58 / патроны-масштаб @0x11240 / валидация статуса @0x58f18.
// Поток 54 бита: [5×1 жив][14 маска оружия id1..14][5×3 патроны-бакет][6 HP][6 статус][8 чексумма].
// Чексумма: 5 групп по 8 бит (rol7) + группа 6 бит (rol5), word-сумма, младшие 8 бит.
// Патроны: бакет=(units·8−1)/max; восстановление units=((2v+1)·max)>>12 (units: штуки = ROM>>8).
// HP: восстановление hp=((2v+1)·100)>>7 (хранится 0..63). Статус: ep·16+этаж; ROM проверяет
// таблицей 58f18 по (status+1). Чит-слова: Highrise! (фулл-лоадаут, эп2) / Basement! (эп3) /
// Boxing!!! (кулаки, статус 0x1F). Пароль = 9 символов.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace ztpass {

// Алфавит ROM @0x585ea (64 симв). Индекс = позиция (58f58).
inline const char* ALPHABET = "xB?DEjGH)JKLMNOPRSTUVQWXYZabefcdhigFk!mnopqrstuvwyAz27453968C/-*";
// Перестановка бит @0x5862a: dst[PERM[i]] = src[i], i=0..53.
inline const uint8_t PERM[54] = {
    0x2e,0x28,0x16,0x05,0x12,0x0e,0x2f,0x26,0x13,0x1d,0x06,0x0f,0x30,0x29,0x27,0x1e,
    0x07,0x10,0x31,0x2a,0x25,0x17,0x08,0x11,0x22,0x14,0x18,0x1f,0x09,0x00,0x32,0x2b,
    0x19,0x20,0x0a,0x01,0x33,0x23,0x1a,0x21,0x0b,0x02,0x34,0x2c,0x15,0x1b,0x0c,0x03,
    0x35,0x2d,0x24,0x1c,0x0d,0x04 };
// Максимумы патронов (ROM units, @0x11240; штуки = >>8): id0..14.
inline const uint16_t MAX_AMMO[15] = { 256,2560,25344,2560,2560,2560,2560,25344,25344,2560,25344,25344,25344,2560,25344 };
// Валидация статуса @0x58f18 (индекс status+1, 64 байта).
inline const uint8_t STATUS_OK[64] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,0, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
    1,1,1,1,1,1,1,1,1,1,1,1,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

struct PwState {
    bool alive[5]   = {true,true,true,true,true};   // −5714..−5710 (1=жив)
    int  weapons[5] = {0,0,0,0,0};                  // weapon-id 5 слотов (0=пусто), −570f..−570b
    int  ammoN[5]   = {0,0,0,0,0};                  // патроны В ШТУКАХ соответствующих слотов
    int  hp         = 99;                           // проценты 0..99
    int  status     = 0;                            // 6 бит: ep·16 + этаж
    bool cheatFull  = false;                        // чит-слово: фулл-лоадаут применён
};

// ── бит-примитивы (58660/5868a: LSB-first) ──
struct Bits { uint8_t b[16] = {0}; };
inline void putBit(Bits& s, int pos, int bit) { if (bit) s.b[pos>>3] |= (uint8_t)(1<<(pos&7)); }
inline int  getBit(const Bits& s, int pos) { return (s.b[pos>>3] >> (pos&7)) & 1; }
inline void putN(Bits& s, int& pos, unsigned v, int n) { for (int i = 0; i < n; ++i) putBit(s, pos++, (v>>i)&1); }
inline unsigned getN(const Bits& s, int& pos, int n) { unsigned v = 0; for (int i = 0; i < n; ++i) v |= (unsigned)getBit(s, pos++) << i; return v; }
// XOR-поток шифра (586c6): d3=0x56CA2D69; байт ^= d3; d3 = swap16(ror32(d3,7)).
inline void xorStream(uint8_t* p, int n) {
    uint32_t d3 = 0x56CA2D69u;
    for (int i = 0; i < n; ++i) {
        p[i] ^= (uint8_t)d3;
        d3 = (d3 >> 7) | (d3 << 25);
        d3 = (d3 >> 16) | (d3 << 16);
    }
}
// Чексумма (58832): word-сумма 6 групп LSB-first (5×8 бит + 6 бит).
inline uint16_t checksum(const Bits& s) {
    int pos = 0; uint32_t sum = 0;
    for (int g = 0; g < 5; ++g) sum += getN(s, pos, 8);
    sum += getN(s, pos, 6);
    return (uint16_t)sum;
}

// ── ГЕНЕРАТОР (58720): состояние → 9 символов ──
inline std::string encode(const PwState& st) {
    Bits raw; int pos = 0;
    for (int i = 0; i < 5; ++i) putN(raw, pos, st.alive[i] ? 1 : 0, 1);
    unsigned mask = 0;
    for (int i = 0; i < 5; ++i) if (st.weapons[i] > 0 && st.weapons[i] <= 14) mask |= 1u << st.weapons[i];
    putN(raw, pos, mask >> 1, 14);                                  // биты id1..14
    for (int i = 0; i < 5; ++i) {                                   // бакет патронов (57f3e): (units·8−1)/max
        int id = st.weapons[i]; unsigned v = 0;
        if (id > 0 && id <= 14 && st.ammoN[i] > 0) {
            long units = (long)st.ammoN[i] << 8;
            long b = (units * 8 - 1) / MAX_AMMO[id];
            v = (unsigned)(b < 0 ? 0 : b > 7 ? 7 : b);
        }
        putN(raw, pos, v, 3);
    }
    unsigned hv = (unsigned)((st.hp < 0 ? 0 : st.hp > 99 ? 99 : st.hp) * 64 / 100);
    if (hv > 63) hv = 63;
    putN(raw, pos, hv, 6);
    putN(raw, pos, (unsigned)st.status & 0x3F, 6);
    putN(raw, pos, checksum(raw), 8);                               // 46+8 = 54 бита
    Bits enc;                                                       // перестановка (586b4)
    for (int i = 0; i < 54; ++i) putBit(enc, PERM[i], getBit(raw, i));
    xorStream(enc.b, 7);
    std::string out; int p2 = 0;
    for (int c = 0; c < 9; ++c) out += ALPHABET[getN(enc, p2, 6)];
    return out;
}

// ── ВАЛИДАТОР (58a74): 9 символов → состояние. true = ок. ──
inline bool decode(const char* pw, PwState& st) {
    size_t n = std::strlen(pw);
    if (n != 9) return false;
    auto full = [&](int stat) {                                     // чит-сеттер 5897a
        st = PwState{};
        st.weapons[0]=1; st.weapons[1]=3; st.weapons[2]=0xb; st.weapons[3]=0xc; st.weapons[4]=0xe;
        for (int i = 0; i < 5; ++i) { int id = st.weapons[i]; st.ammoN[i] = (int)(((15L*MAX_AMMO[id])>>12)); }
        st.hp = 99; st.status = stat; st.cheatFull = true;
    };
    if (!std::strcmp(pw, "Highrise!")) { full(0x10); return true; } // эп2 этаж0
    if (!std::strcmp(pw, "Basement!")) { full(0x20); return true; } // эп3 этаж0
    if (!std::strcmp(pw, "Boxing!!!")) {                            // кулаки, статус 0x1F
        st = PwState{}; st.hp = 99; st.status = 0x1F; return true;
    }
    Bits enc; int pos = 0;
    for (int c = 0; c < 9; ++c) {                                   // 58f58: символ → индекс алфавита
        const char* f = std::strchr(ALPHABET, pw[c]);
        if (!f) return false;
        putN(enc, pos, (unsigned)(f - ALPHABET), 6);
    }
    xorStream(enc.b, 7);                                            // 586e8: XOR → обратная перестановка
    Bits raw;
    for (int i = 0; i < 54; ++i) putBit(raw, i, getBit(enc, PERM[i]));
    { int cp = 46; unsigned stored = getN(raw, cp, 8);              // чексумма (58cc6): младший байт
      if ((uint8_t)stored != (uint8_t)checksum(raw)) return false; }
    int p = 0;
    for (int i = 0; i < 5; ++i) st.alive[i] = getN(raw, p, 1) != 0;
    unsigned mask = getN(raw, p, 14);                               // 58d6a: маска → слоты (>5 оружий = FAIL)
    int slot = 0;
    for (int id = 1; id <= 14; ++id) if (mask & (1u << (id - 1))) {
        if (slot >= 5) return false;
        st.weapons[slot++] = id;
    }
    for (; slot < 5; ++slot) st.weapons[slot] = 0;
    for (int i = 0; i < 5; ++i) {                                   // патроны: ((2v+1)·max)>>12 (58e40)
        unsigned v = getN(raw, p, 3);
        int id = st.weapons[i];
        st.ammoN[i] = (id > 0 && id <= 14) ? (int)(((2L*v+1) * MAX_AMMO[id]) >> 12) : 0;
    }
    { unsigned v = getN(raw, p, 6); st.hp = (int)(((2L*v+1) * 100) >> 7); }   // 58eea
    st.status = (int)getN(raw, p, 6);
    if (!STATUS_OK[(st.status + 1) & 0x3F]) return false;           // 58f02: валидация таблицей
    st.cheatFull = false;
    return true;
}

} // namespace ztpass
