#include "../include/cpu/z80.h"
#include "cpu/z80_cb.h"
#include "cpu/z80_dd.h"

// ============================================================================
// Dispatcher XY CB — (IX+d ou IY+d) avec opérations CB
// ireg pointe vers IX ou IY selon le préfixe appelant
// ============================================================================

int z80_exec_xycb(Z80* cpu, uint16_t* ireg) {
    // Lire le déplacement signé après DD/FD CB
    int8_t disp = static_cast<int8_t>(z80_fetch_byte(cpu));
    
    // Lire l'opcode CB — R incrémenté par z80_fetch_byte (2 fetches = 2 incréments de R)
    uint8_t opcode = z80_fetch_byte(cpu);
    // NE PAS incrémenter R manuellement ici — z80_fetch_byte le fait déjà!

    uint8_t x = (opcode >> 6) & 3;   // Catégorie: 0=rot, 1=bit, 2=res, 3=set
    uint8_t y = (opcode >> 3) & 7;   // Opération spécifique
    uint8_t z = opcode & 7;           // Registre cible

    int cycles_base = 23; // (IX+d)/(IY+d) = 23 T-states pour DDCB/FDCB (4 préfixe + 19 mémoire)

    switch (x) {
    case 0: { // Rotations/Décalages — BUG E : cast disp via int32_t pour cohérence
        uint8_t val;
        if (z == 6) {
            val = cpu->mem_read_fn(static_cast<uint16_t>(
                static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp)));
        } else {
            switch (z) {
                case 0: val = cpu->B; break;
                case 1: val = cpu->C; break;
                case 2: val = cpu->D; break;
                case 3: val = cpu->E; break;
                case 4: val = static_cast<uint8_t>((*ireg >> 8) & 0xFF); break;
                case 5: val = static_cast<uint8_t>(*ireg & 0xFF); break;
                case 7: val = cpu->A; break;
                default: val = 0; break;
            }
        }

        uint8_t result;
        
        switch (y) {
        case 0: result = static_cast<uint8_t>(((val << 1) | (val >> 7)) & 0xFF); break;   // RLC
        case 1: result = static_cast<uint8_t>((val >> 1) | ((val & 1) ? 0x80 : 0)); break; // RRC
        case 2: { bool old_carry = (cpu->F & FLAG_C) != 0; result = static_cast<uint8_t>(((val << 1) | (old_carry ? 1 : 0)) & 0xFF); break; } // RL
        case 3: { bool old_carry = (cpu->F & FLAG_C) != 0; result = static_cast<uint8_t>((val >> 1) | (old_carry ? 0x80 : 0)); break; } // RR
        case 4: result = static_cast<uint8_t>((val << 1) & 0xFF); break;                   // SLA
        case 5: { bool msb = val & 0x80; result = static_cast<uint8_t>((val >> 1) | msb); break; } // SRA
        case 6: result = static_cast<uint8_t>(((val << 1) | 0x01) & 0xFF); break;          // SLL (undoc)
        case 7: result = static_cast<uint8_t>(val >> 1); break;                            // SRL
        default: result = 0; break;
        }

        // Appliquer les flags comme CB normal
        cpu->F = 0;
        if (result == 0) cpu->F |= FLAG_Z;
        if (result & 0x80) cpu->F |= FLAG_S;
        
        // Calculer Carry selon l'opération
        bool carry_out = false;
        switch (y) {
            case 0: carry_out = (val & 0x80) != 0; break;   // RLC
            case 1: carry_out = (val & 1) != 0; break;       // RRC
            case 2: carry_out = (val & 0x80) != 0; break;    // RL
            case 3: carry_out = (val & 1) != 0; break;       // RR
            case 4: carry_out = (val & 0x80) != 0; break;    // SLA
            case 5: carry_out = (val & 1) != 0; break;       // SRA
            case 6: carry_out = (val & 0x80) != 0; break;    // SLL
            case 7: carry_out = (val & 1) != 0; break;       // SRL
        }
        cpu->F |= carry_out ? FLAG_C : 0;
        cpu->F |= z80_parity(result) ? FLAG_PV : 0;
        z80_set_flags_xy(cpu, result);

        // Bug FIX rapport v6 #D : Écrire TOUJOURS en mémoire d'abord (instructions non doc)
        uint16_t target_addr = static_cast<uint16_t>(
            static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp));
        cpu->mem_write_fn(target_addr, result);

        // Si z != 6, on copie AUSSI dans le registre (instruction non doc)
        if (z != 6) {
            switch (z) {
                case 0: cpu->B = result; break;
                case 1: cpu->C = result; break;
                case 2: cpu->D = result; break;
                case 3: cpu->E = result; break;
                case 4: *ireg = (*ireg & 0x00FF) | static_cast<uint16_t>(result << 8); break;
                case 5: *ireg = (*ireg & 0xFF00) | result; break;
                case 7: cpu->A = result; break;
                default: break;
            }
        }
        return cycles_base;
    }

    case 1: { // BIT n, r — BUG E : cast disp via int32_t pour cohérence
        uint8_t val;
        if (z == 6) {
            val = cpu->mem_read_fn(static_cast<uint16_t>(
                static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp)));
        } else {
            switch (z) {
                case 0: val = cpu->B; break;
                case 1: val = cpu->C; break;
                case 2: val = cpu->D; break;
                case 3: val = cpu->E; break;
                case 4: val = static_cast<uint8_t>((*ireg >> 8) & 0xFF); break;
                case 5: val = static_cast<uint8_t>(*ireg & 0xFF); break;
                case 7: val = cpu->A; break;
                default: val = 0; break;
            }
        }

        bool bit_set = ((val >> y) & 1) != 0;
        
        cpu->F &= ~(FLAG_Z | FLAG_H | FLAG_PV);
        if (!bit_set) cpu->F |= FLAG_Z;
        cpu->F |= FLAG_H; // Half Carry toujours set après BIT
        
        // P/V = 1 si bit non set (pour BIT)
        cpu->F |= !bit_set ? FLAG_PV : 0;
        
        // Flags S, X, Y basés sur la valeur originale
        if (val & 0x80) cpu->F |= FLAG_S;
        z80_set_flags_xy(cpu, val);

        return 15; // BIT sur (IX+d)/(IY+d) = 15 T-states
    }

    case 2: { // RES n, r — Bug FIX rapport v6 #D : écriture miroir mémoire toujours faite
        uint8_t val;
        if (z == 6) {
            val = cpu->mem_read_fn(static_cast<uint16_t>(
                static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp)));
        } else {
            switch (z) {
                case 0: val = cpu->B; break;
                case 1: val = cpu->C; break;
                case 2: val = cpu->D; break;
                case 3: val = cpu->E; break;
                case 4: val = static_cast<uint8_t>((*ireg >> 8) & 0xFF); break;
                case 5: val = static_cast<uint8_t>(*ireg & 0xFF); break;
                case 7: val = cpu->A; break;
                default: val = 0; break;
            }
        }

        uint8_t result = val & ~(1 << y);

        // Bug FIX rapport v6 #D : Écrire TOUJOURS en mémoire d'abord (instructions non doc)
        uint16_t target_addr = static_cast<uint16_t>(
            static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp));
        cpu->mem_write_fn(target_addr, result);

        // Si z != 6, on copie AUSSI dans le registre (instruction non doc)
        if (z != 6) {
            switch (z) {
                case 0: cpu->B = result; break;
                case 1: cpu->C = result; break;
                case 2: cpu->D = result; break;
                case 3: cpu->E = result; break;
                case 4: *ireg = (*ireg & 0x00FF) | static_cast<uint16_t>(result << 8); break;
                case 5: *ireg = (*ireg & 0xFF00) | result; break;
                case 7: cpu->A = result; break;
                default: break;
            }
        }
        return cycles_base;
    }

    case 3: { // SET n, r — Bug FIX rapport v6 #D : écriture miroir mémoire toujours faite
        uint8_t val;
        if (z == 6) {
            val = cpu->mem_read_fn(static_cast<uint16_t>(
                static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp)));
        } else {
            switch (z) {
                case 0: val = cpu->B; break;
                case 1: val = cpu->C; break;
                case 2: val = cpu->D; break;
                case 3: val = cpu->E; break;
                case 4: val = static_cast<uint8_t>((*ireg >> 8) & 0xFF); break;
                case 5: val = static_cast<uint8_t>(*ireg & 0xFF); break;
                case 7: val = cpu->A; break;
                default: val = 0; break;
            }
        }

        uint8_t result = val | (1 << y);

        // Bug FIX rapport v6 #D : Écrire TOUJOURS en mémoire d'abord (instructions non doc)
        uint16_t target_addr = static_cast<uint16_t>(
            static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp));
        cpu->mem_write_fn(target_addr, result);

        // Si z != 6, on copie AUSSI dans le registre (instruction non doc)
        if (z != 6) {
            switch (z) {
                case 0: cpu->B = result; break;
                case 1: cpu->C = result; break;
                case 2: cpu->D = result; break;
                case 3: cpu->E = result; break;
                case 4: *ireg = (*ireg & 0x00FF) | static_cast<uint16_t>(result << 8); break;
                case 5: *ireg = (*ireg & 0xFF00) | result; break;
                case 7: cpu->A = result; break;
                default: break;
            }
        }
        return cycles_base;
    }

    default:
        return 8; // NOP fallback
    }
}