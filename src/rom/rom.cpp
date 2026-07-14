// ztpp — src/rom/rom.cpp: определения доступа к ROM (загрузка файла + LUT углов).
#include "rom.hpp"
#include <fstream>

bool Rom::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0) return false;
    f.seekg(0);
    data.resize(static_cast<size_t>(n));
    return static_cast<bool>(f.read(reinterpret_cast<char*>(data.data()), n));
}

int16_t* zAngLUT()   { static int16_t t[1024]; return t; }
bool&    zAngLUTok() { static bool b = false; return b; }

void loadZAngLUT(const Rom& rom) {
    for (int a = 0; a < 512; ++a) {
        zAngLUT()[a * 2]     = static_cast<int16_t>(rom.u16(0x8124 + static_cast<size_t>(a) * 4));
        zAngLUT()[a * 2 + 1] = static_cast<int16_t>(rom.u16(0x8124 + static_cast<size_t>(a) * 4 + 2));
    }
    zAngLUTok() = true;
}
