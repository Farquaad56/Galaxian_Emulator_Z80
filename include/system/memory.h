#ifndef MEMORY_H
#define MEMORY_H

#include "cpu/z80.h"
#include <cstdint>
#include <vector>

// ============================================================================
// Mémoire Z80 - 64 Ko (espace d'adressage 16 bits)
// ============================================================================

class Z80Memory {
public:
    Z80Memory();
    
    /**
 * Lire un octet à l'adresse spécifiée.
 */
    uint8_t read(uint16_t addr);
    
    /**
     * Écrire un octet à l'adresse spécifiée.
     */
    void write(uint16_t addr, uint8_t value);
    
    /**
     * Charger des données dans la mémoire à une adresse donnée.
     */
    void load(uint16_t address, const uint8_t* data, size_t length);
    
    /**
     * Charger un fichier binaire en mémoire (début à 0x0000).
     */
    bool load_binary(const char* filename);
    
    /**
     * Accéder aux données brutes (pour débogage).
     */
    inline uint8_t* data() { return data_.data(); }
    inline size_t size() const { return data_.size(); }

private:
    std::vector<uint8_t> data_;  // 65536 bytes = 64 KB
};

#endif // MEMORY_H