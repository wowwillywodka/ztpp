// ztpp — Zero Tolerance C++ port (prototype)
// ROM access: загрузка + big-endian чтение (68k).
#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>

struct Rom {
    std::vector<uint8_t> data;

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return false;
        std::streamsize n = f.tellg();
        if (n <= 0) return false;
        f.seekg(0);
        data.resize(static_cast<size_t>(n));
        return static_cast<bool>(f.read(reinterpret_cast<char*>(data.data()), n));
    }

    size_t   size()        const { return data.size(); }
    uint8_t  u8 (size_t a)  const { return a < data.size() ? data[a] : 0; }
    uint16_t u16(size_t a)  const { return static_cast<uint16_t>((u8(a) << 8) | u8(a + 1)); } // BE
    uint32_t u32(size_t a)  const { return (static_cast<uint32_t>(u16(a)) << 16) | u16(a + 2); }
};
