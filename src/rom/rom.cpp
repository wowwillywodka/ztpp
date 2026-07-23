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

void loadZAngLUT(const Rom& rom, size_t base) {
    for (int a = 0; a < 512; ++a) {
        zAngLUT()[a * 2]     = static_cast<int16_t>(rom.u16(base + static_cast<size_t>(a) * 4));
        zAngLUT()[a * 2 + 1] = static_cast<int16_t>(rom.u16(base + static_cast<size_t>(a) * 4 + 2));
    }
    zAngLUTok() = true;
}

int16_t* zFanLUT()   { static int16_t t[129]; return t; }
bool&    zFanLUTok() { static bool b = false; return b; }

void loadZFanLUT(const Rom& rom, size_t base) {
    for (int i = 0; i <= 128; ++i)
        zFanLUT()[i] = static_cast<int16_t>(rom.u16(base + static_cast<size_t>(i) * 2));
    // сигнатура атан-веера (защита от неверной базы у др. билдов): края ±64, центр 0
    zFanLUTok() = (zFanLUT()[0] == -64 && zFanLUT()[64] == 0 && zFanLUT()[128] == 64);
}
