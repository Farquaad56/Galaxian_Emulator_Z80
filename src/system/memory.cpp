#include "../include/system/memory.h"
#include <fstream>
#include <iostream>

Z80Memory::Z80Memory() : data_(65536, 0x00) {
    // Mémoire initialisée à 0x00
}

uint8_t Z80Memory::read(uint16_t addr) {
    return data_[addr];
}

void Z80Memory::write(uint16_t addr, uint8_t value) {
    data_[addr] = value;
}

void Z80Memory::load(uint16_t address, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (address + i < data_.size()) {
            data_[address + i] = data[i];
        }
    }
}

bool Z80Memory::load_binary(const char* filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        return false;
    }
    
    size_t size = file.tellg();
    if (size > 65536) {
        std::cerr << "File too large for Z80 memory: " << size << " bytes" << std::endl;
        return false;
    }
    
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(data_.data()), size);
    file.close();
    
    return true;
}

