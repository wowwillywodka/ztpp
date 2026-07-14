// ztpp — доступ к ROM: загрузка + big-endian чтение (68k).
//
// МОДУЛЬ src/rom — декодер ROM→ассеты. НЕ зависит от рендера/SDL/игровой логики; переносится
// целиком в будущий движок как «код-мост» (см. план: reference-порт даёт спецификацию + декодеры).
// Заголовок = объявления; тяжёлые определения — в rom.cpp. Горячие аксессоры (u8/u16/u32) —
// inline в заголовке (вызываются миллионами в рейкастере).
#pragma once
#include <cstdint>
#include <vector>
#include <string>

struct Rom {
    std::vector<uint8_t> data;

    bool load(const std::string& path);                        // rom.cpp (файловый I/O)

    size_t   size()        const { return data.size(); }
    uint8_t  u8 (size_t a)  const { return a < data.size() ? data[a] : 0; }
    uint16_t u16(size_t a)  const { return static_cast<uint16_t>((u8(a) << 8) | u8(a + 1)); } // BE
    uint32_t u32(size_t a)  const { return (static_cast<uint32_t>(u16(a)) << 16) | u16(a + 2); }
};

// LUT углов ZT @ROM 0x8124: 512 записей ×4 байта = {cos, sin} ×256 (= dirX·256, dirY·256). МД-угол a0 = −Y.
// Используется целочисленным рендером стен (fixed-point) для бит-в-бит проекции как МД (реальные значения ROM,
// а не round(cos·256) — отличаются на ±1). Загружается из ROM в gamedata, читается в faithful.hpp.
int16_t* zAngLUT();     // [a*2]=cos(dirX·256), [a*2+1]=sin(dirY·256)
bool&    zAngLUTok();
void     loadZAngLUT(const Rom& rom);
