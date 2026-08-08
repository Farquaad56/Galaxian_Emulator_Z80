#include "../include/cpu/z80.h"
#include "../include/cpu/z80_ops.h"
#include "../include/cpu/z80_cb.h"
#include "../include/cpu/z80_ed.h"
#include "../include/cpu/z80_dd.h"

// Forward declarations
int z80_exec_ed(Z80* cpu);
int z80_exec_dd(Z80* cpu);

// ============================================================================
// ADD IY,rp — correction : préserver S, Z, PV ; effacer H, N, C, X, Y
// ============================================================================

static inline int add_iy_rp(Z80* cpu, uint16_t rp) {
    uint32_t iy32     = static_cast<uint32_t>(cpu->IY);
    uint32_t result   = iy32 + rp;
    bool carry_out    = (result > 0xFFFF);
    bool half_carry   = ((iy32 & 0xFFF) + (rp & 0xFFF)) > 0xFFF;

    // Préserver S, Z, PV ; effacer H, N, C, X, Y
    cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV);
    if (carry_out)   cpu->F |= FLAG_C;
    if (half_carry)  cpu->F |= FLAG_H;
    // N = 0 (déjà effacé)
    z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
    cpu->IY = static_cast<uint16_t>(result);
    return 15;  // ADD IY,rp avec préfixe FD = 15 T-states
}

// ============================================================================
// Décodeur principal FD — identique à DD mais avec IY au lieu de IX
// Bugs corrigés : #B (1 octet), #C (IYH/IYL corrects), #D (LD IY,(nn)), 
//                 #E (19 T-states), #F (FD CB refactorisé), #L (LD IYH,IYL)
// ============================================================================

int z80_exec_fd(Z80* cpu) {
    uint8_t opcode = z80_fetch_byte(cpu);
    cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);

    // Double préfixe : FD FD, FD ED, FD DD → ignorer et continuer
    if (opcode == 0xFD || opcode == 0xED || opcode == 0xDD) {
        cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
        // Re-fetch le vrai opcode et traiter normalement
        if (opcode == 0xED) return z80_exec_ed(cpu) + 4;
        if (opcode == 0xDD) {
            uint8_t real_opcode = z80_fetch_byte(cpu);
            cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
            return 4 + z80_exec_dd(cpu);
        }
        // FD FD → NOP supplémentaire
        return 4;
    }

    // Préfixe FD CB : réutiliser z80_exec_xycb avec IY à la place de IX
    if (opcode == 0xCB) {
        return z80_exec_xycb(cpu, &cpu->IY);
    }

    // ===== Toutes les instructions avec substitution HL→IY =====
    switch (opcode) {
        // ==================== NOP / DJNZ ====================
        case 0x00: return 4;  // NOP
        case 0x10: { // DJNZ d
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            cpu->B--;
            if (cpu->B != 0) {
                cpu->PC += d;
                cpu->WZ = cpu->PC;
                return 13;
            }
            return 8;
        }

        // ==================== INC/DEC r ====================
        case 0x04: { z80_inc_r(cpu, 0); return 4; }   // INC B
        case 0x05: { z80_dec_r(cpu, 0); return 5; }   // DEC B
        case 0x0C: { z80_inc_r(cpu, 1); return 4; }   // INC C
        case 0x0D: { z80_dec_r(cpu, 1); return 5; }   // DEC C
        case 0x14: { z80_inc_r(cpu, 2); return 4; }   // INC D
        case 0x15: { z80_dec_r(cpu, 2); return 5; }   // DEC D
        case 0x1C: { z80_inc_r(cpu, 3); return 4; }   // INC E
        case 0x1D: { z80_dec_r(cpu, 3); return 5; }   // DEC E

        // ==================== INC/DEC IYH/IYL — undoc (Bug #L FIX) ====================
        case 0x24: { // INC IYH
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            uint16_t result = val + 1;
            bool half_carry = ((val & 0xF) + 1) > 0xF;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_carry)        cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(result << 8);
            return 4;
        }
        case 0x25: { // DEC IYH
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            int16_t result = val - 1;
            bool half_borrow = ((val & 0xF) - 1) < 0;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_N | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_borrow)       cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(static_cast<uint8_t>(result) << 8);
            return 5;
        }
        case 0x2C: { // INC IYL
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            uint16_t result = val + 1;
            bool half_carry = ((val & 0xF) + 1) > 0xF;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_carry)        cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IY = (cpu->IY & 0xFF00) | result;
            return 4;
        }
        case 0x2D: { // DEC IYL
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            int16_t result = val - 1;
            bool half_borrow = ((val & 0xF) - 1) < 0;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_N | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_borrow)       cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IY = (cpu->IY & 0xFF00) | static_cast<uint8_t>(result);
            return 5;
        }

        // ==================== INC/DEC (IY+d) — Bug #E FIX : 19 T-states ====================
        case 0x34: { // INC (IY+d) — 19 T-states
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            uint8_t val = cpu->mem_read_fn(addr);
            uint8_t result = static_cast<uint8_t>(val + 1);
            bool half_carry = ((val & 0xF) + 1) > 0xF;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0) cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (half_carry) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, result);
            cpu->mem_write_fn(addr, result);
            return 19;
        }
        case 0x35: { // DEC (IY+d) — 23 T-states
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            uint8_t val = cpu->mem_read_fn(addr);
            uint8_t result = static_cast<uint8_t>(val - 1);
            bool half_borrow = ((val & 0xF) - 1) < 0;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_N | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0) cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (half_borrow) cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, result);
            cpu->mem_write_fn(addr, result);
            return 23;
        }

        case 0x3C: { z80_inc_r(cpu, 7); return 4; }   // INC A
        case 0x3D: { z80_dec_r(cpu, 7); return 5; }   // DEC A

        // ==================== LD r,n / LD (IY+d),n — Bug #B FIX : 1 octet seulement ====================
        case 0x06: { cpu->B = z80_fetch_byte(cpu); return 7; }   // LD B,n
        case 0x0E: { cpu->C = z80_fetch_byte(cpu); return 7; }   // LD C,n
        case 0x16: { cpu->D = z80_fetch_byte(cpu); return 7; }   // LD D,n
        case 0x1E: { cpu->E = z80_fetch_byte(cpu); return 7; }   // LD E,n

        // Bug #B FIX : LD IYH,n (FD 26) — lire 1 octet seulement, pas 2
        case 0x26: {
            uint8_t n = z80_fetch_byte(cpu);
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(n << 8);
            return 11;
        }

        // Bug #B FIX : LD IYL,n (FD 2E) — lire 1 octet seulement, pas 2
        case 0x2E: {
            uint8_t n = z80_fetch_byte(cpu);
            cpu->IY = (cpu->IY & 0xFF00) | n;
            return 11;
        }

        // Bug #E FIX : LD (IY+d),n — 19 T-states
        case 0x36: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint8_t val = z80_fetch_byte(cpu);
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->mem_write_fn(addr, val);
            return 19;
        }

        case 0x3E: { cpu->A = z80_fetch_byte(cpu); return 7; }   // LD A,n

        // ==================== LD r,r (avec substitution IYH/IYL) ====================
        case 0x40: /* LD B,B */ break;
        case 0x41: { cpu->B = cpu->C; } break;
        case 0x42: { cpu->B = cpu->D; } break;
        case 0x43: { cpu->B = cpu->E; } break;
        case 0x44: { cpu->B = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF); } break;   // LD B,IYH
        case 0x45: { cpu->B = static_cast<uint8_t>(cpu->IY & 0xFF); } break;          // LD B,IYL

        // Bug #E FIX : LD B,(IY+d) — 19 T-states
        case 0x46: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->B = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x47: { cpu->B = cpu->A; } break;
        case 0x48: { cpu->C = cpu->B; } break;
        case 0x49: /* LD C,C */ break;
        case 0x4A: { cpu->C = cpu->D; } break;
        case 0x4B: { cpu->C = cpu->E; } break;
        case 0x4C: { cpu->C = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF); } break;   // LD C,IYH
        case 0x4D: { cpu->C = static_cast<uint8_t>(cpu->IY & 0xFF); } break;          // LD C,IYL

        // Bug #E FIX : LD C,(IY+d) — 19 T-states
        case 0x4E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->C = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x4F: { cpu->C = cpu->A; } break;
        case 0x50: { cpu->D = cpu->B; } break;
        case 0x51: { cpu->D = cpu->C; } break;
        case 0x52: /* LD D,D */ break;
        case 0x53: { cpu->D = cpu->E; } break;
        case 0x54: { cpu->D = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF); } break;   // LD D,IYH
        case 0x55: { cpu->D = static_cast<uint8_t>(cpu->IY & 0xFF); } break;          // LD D,IYL

        // Bug #E FIX : LD D,(IY+d) — 19 T-states
        case 0x56: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->D = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x57: { cpu->D = cpu->A; } break;
        case 0x58: { cpu->E = cpu->B; } break;
        case 0x59: { cpu->E = cpu->C; } break;
        case 0x5A: { cpu->E = cpu->D; } break;
        case 0x5B: /* LD E,E */ break;
        case 0x5C: { cpu->E = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF); } break;   // LD E,IYH
        case 0x5D: { cpu->E = static_cast<uint8_t>(cpu->IY & 0xFF); } break;          // LD E,IYL

        // Bug #E FIX : LD E,(IY+d) — 19 T-states
        case 0x5E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->E = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x5F: { cpu->E = cpu->A; } break;

        // ==================== LD IYH,* / LD IYL,* — Bug #C FIX + Bug #L FIX ====================
        case 0x60: { /* LD IYH,B */
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(cpu->B << 8);
            return 4; }
        case 0x61: { /* LD IYH,C */
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(cpu->C << 8);
            return 4; }
        case 0x62: { /* LD IYH,D */
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(cpu->D << 8);
            return 4; }
        case 0x63: { /* LD IYH,E */
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(cpu->E << 8);
            return 4; }
        case 0x64: /* LD IYH,IYH */ break;

        // Bug #L FIX : LD IYH,IYL — placer IYL dans IYH (calcul correct)
        case 0x65: {
            uint8_t iyl = static_cast<uint8_t>(cpu->IY & 0xFF);
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(iyl << 8);
            return 4;
        }

        // Bug #E FIX : LD H,(IY+d) — écrit dans H, pas IYH !
        case 0x66: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->H = cpu->mem_read_fn(addr);
            return 19;
        }

        // Bug #E FIX : LD IYH,A — pas de déplacement !
        case 0x67: {
            cpu->IY = (cpu->IY & 0x00FF) | static_cast<uint16_t>(cpu->A << 8);
            return 4;
        }

        // ==================== LD IYL,* — Bug #C FIX ====================
        case 0x68: { /* LD IYL,B */
            cpu->IY = (cpu->IY & 0xFF00) | cpu->B;
            return 4; }
        case 0x69: { /* LD IYL,C */
            cpu->IY = (cpu->IY & 0xFF00) | cpu->C;
            return 4; }
        case 0x6A: { /* LD IYL,D */
            cpu->IY = (cpu->IY & 0xFF00) | cpu->D;
            return 4; }
        case 0x6B: { /* LD IYL,E */
            cpu->IY = (cpu->IY & 0xFF00) | cpu->E;
            return 4; }
        case 0x6C: { // LD IYL,IYH
            uint8_t iyh = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            cpu->IY = (cpu->IY & 0xFF00) | iyh;
            return 4;
        }
        case 0x6D: /* LD IYL,IYL */ break;

        // Bug #E FIX : LD L,(IY+d) — écrit dans L, pas IYL !
        case 0x6E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->L = cpu->mem_read_fn(addr);
            return 19;
        }

        // Bug #E FIX : LD IYL,A — pas de déplacement !
        case 0x6F: {
            cpu->IY = (cpu->IY & 0xFF00) | cpu->A;
            return 4;
        }

        case 0x78: { cpu->A = cpu->B; } break;
        case 0x79: { cpu->A = cpu->C; } break;
        case 0x7A: { cpu->A = cpu->D; } break;
        case 0x7B: { cpu->A = cpu->E; } break;
        case 0x7C: { cpu->A = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF); } break;   // LD A,IYH
        case 0x7D: { cpu->A = static_cast<uint8_t>(cpu->IY & 0xFF); } break;          // LD A,IYL

        // Bug FIX : LD (IY+d),A — 19 T-states (manquant dans le code original)
        case 0x77: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->A);
            return 19;
        }

        // Bug #E FIX : LD A,(IY+d) — 19 T-states (cast via int32_t pour cohérence)
        case 0x7E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IY + d);
            cpu->A = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x7F: /* LD A,A */ break;

        // ==================== ADD IY,rp / LD (nn),IY / LD IY,(nn) — Bug #D FIX ====================
        case 0x01: { cpu->BC = z80_fetch_word(cpu); return 10; }   // LD BC,nn
        case 0x11: { cpu->DE = z80_fetch_word(cpu); return 10; }   // LD DE,nn

        // Bug FIX rapport v4 #H : LD IY,nn (FD 21) — 14 T-states avec préfixe FD
        case 0x21: {
            uint8_t lo = z80_fetch_byte(cpu);
            uint8_t hi = z80_fetch_byte(cpu);
            cpu->IY = static_cast<uint16_t>((hi << 8) | lo);
            return 14;
        }

        case 0x31: { cpu->SP = z80_fetch_word(cpu); return 10; }   // LD SP,nn

        case 0x09: { /* ADD IY,BC */
            uint32_t result = static_cast<uint32_t>(cpu->IY) + cpu->BC;
            bool carry_out = (result > 0xFFFF);
            bool half_carry = ((cpu->IY & 0xFFF) + (cpu->BC & 0xFFF)) > 0xFFF;
            cpu->F &= ~(FLAG_C | FLAG_H | FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N);
            if (result & 0x8000) cpu->F |= FLAG_S;
            if ((result & 0xFFFF) == 0) cpu->F |= FLAG_Z;
            if (carry_out) cpu->F |= FLAG_C;
            if (half_carry) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
            cpu->IY = static_cast<uint16_t>(result);
            return 11; }
        case 0x19: { /* ADD IY,DE */
            uint32_t result = static_cast<uint32_t>(cpu->IY) + cpu->DE;
            bool carry_out = (result > 0xFFFF);
            bool half_carry = ((cpu->IY & 0xFFF) + (cpu->DE & 0xFFF)) > 0xFFF;
            cpu->F &= ~(FLAG_C | FLAG_H | FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N);
            if (result & 0x8000) cpu->F |= FLAG_S;
            if ((result & 0xFFFF) == 0) cpu->F |= FLAG_Z;
            if (carry_out) cpu->F |= FLAG_C;
            if (half_carry) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
            cpu->IY = static_cast<uint16_t>(result);
            return 11; }
        case 0x29: { /* ADD IY,IY */
            uint32_t result = static_cast<uint32_t>(cpu->IY) * 2;
            bool carry_out = (result > 0xFFFF);
            cpu->F &= ~(FLAG_C | FLAG_H | FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N);
            if (result & 0x8000) cpu->F |= FLAG_S;
            if ((result & 0xFFFF) == 0) cpu->F |= FLAG_Z;
            if (carry_out) cpu->F |= FLAG_C;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
            cpu->IY = static_cast<uint16_t>(result);
            return 11; }
        case 0x39: { /* ADD IY,SP */
            uint32_t result = static_cast<uint32_t>(cpu->IY) + cpu->SP;
            bool carry_out = (result > 0xFFFF);
            bool half_carry = ((cpu->IY & 0xFFF) + (cpu->SP & 0xFFF)) > 0xFFF;
            cpu->F &= ~(FLAG_C | FLAG_H | FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N);
            if (result & 0x8000) cpu->F |= FLAG_S;
            if ((result & 0xFFFF) == 0) cpu->F |= FLAG_Z;
            if (carry_out) cpu->F |= FLAG_C;
            if (half_carry) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
            cpu->IY = static_cast<uint16_t>(result);
            return 11; }

        // Bug #D FIX : LD (nn),IY — écrire IY dans mémoire
        case 0x22: {
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->IY & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->IY >> 8) & 0xFF));
            return 20;
        }

        // Bug #D FIX : LD IY,(nn) — charger dans IY, pas HL
        case 0x2A: {
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->IY = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }

        // Bug FIX rapport v4 #H : INC/DEC IY = 10 T-states avec préfixe FD (pas 6)
        case 0x23: { cpu->IY++; return 10; }   // INC IY
        case 0x2B: { cpu->IY--; return 10; }   // DEC IY

        // ==================== LD (IY+d),r — écriture mémoire via IY+d (Bug FIX) ====================
        case 0x70: { // LD (IY+d),B
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->B);
            return 19; }
        case 0x71: { // LD (IY+d),C
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->C);
            return 19; }
        case 0x72: { // LD (IY+d),D
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->D);
            return 19; }
        case 0x73: { // LD (IY+d),E
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->E);
            return 19; }

        // ==================== LD (BC),A / LD A,(BC) / LD (DE),A / LD A,(DE) ====================
        case 0x02: { cpu->mem_write_fn(cpu->BC, cpu->A); cpu->WZ = cpu->BC + 1; return 7; }   // LD (BC),A
        case 0x0A: { cpu->A = cpu->mem_read_fn(cpu->BC); cpu->WZ = cpu->BC + 1; return 7; }   // LD A,(BC)
        case 0x12: { cpu->mem_write_fn(cpu->DE, cpu->A); cpu->WZ = cpu->DE + 1; return 7; }   // LD (DE),A
        case 0x1A: { cpu->A = cpu->mem_read_fn(cpu->DE); cpu->WZ = cpu->DE + 1; return 7; }   // LD A,(DE)

        // ==================== LD (nn),A / LD A,(nn) ====================
        case 0x32: { /* LD (nn),A — écrire A en mémoire */
            uint16_t addr = z80_fetch_word(cpu);
            cpu->WZ = addr + 1;
            cpu->mem_write_fn(addr, cpu->A);
            return 13; }
        case 0x3A: { /* LD A,(nn) — lire depuis mémoire */
            uint16_t addr = z80_fetch_word(cpu);
            cpu->WZ = addr + 1;
            cpu->A = cpu->mem_read_fn(addr);
            return 13; }

        // ==================== INC/DEC SP / BC / DE ====================
        case 0x03: { cpu->BC++; return 6; }   // INC BC
        case 0x0B: { cpu->BC--; return 6; }   // DEC BC
        case 0x13: { cpu->DE++; return 6; }   // INC DE
        case 0x1B: { cpu->DE--; return 6; }   // DEC DE

        // ==================== PUSH/POP ====================
        case 0xC5: { z80_push_word(cpu, cpu->BC); return 11; }   // PUSH BC
        case 0xD5: { z80_push_word(cpu, cpu->DE); return 11; }   // PUSH DE
        case 0xE5: { z80_push_word(cpu, cpu->IY); return 11; }   // PUSH IY (substitue HL)
        case 0xF5: { z80_push_word(cpu, cpu->AF); return 11; }   // PUSH AF
        case 0xC1: { cpu->BC = z80_pop_word(cpu); return 10; }   // POP BC
        case 0xD1: { cpu->DE = z80_pop_word(cpu); return 10; }   // POP DE
        case 0xE1: { cpu->IY = z80_pop_word(cpu); return 10; }   // POP IY (substitue HL)
        case 0xF1: { /* POP AF — restaurer A et F depuis la pile */
            cpu->AF = z80_pop_word(cpu);
            return 10; }

        // ==================== RST ====================
        case 0xC7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0000; cpu->WZ = 0x0000; return 16; }   // RST 00
        case 0xCF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0008; cpu->WZ = 0x0008; return 16; }   // RST 08
        case 0xD7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0010; cpu->WZ = 0x0010; return 16; }   // RST 10
        case 0xDF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0018; cpu->WZ = 0x0018; return 16; }   // RST 18
        case 0xE7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0020; cpu->WZ = 0x0020; return 16; }   // RST 20
        case 0xEF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0028; cpu->WZ = 0x0028; return 16; }   // RST 28
        case 0xF7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0030; cpu->WZ = 0x0030; return 16; }   // RST 30
        case 0xFF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0038; cpu->WZ = 0x0038; return 16; }   // RST 38

        // ==================== LD SP,IY — opcode FD F9 (comme DD F9 pour LD SP,IX) ====================
        case 0xF9: { /* LD SP,IY */
            cpu->SP = cpu->IY;
            return 6; }

        // ==================== JP / JR / CALL / RET ====================
        case 0xC3: { uint16_t addr = z80_fetch_word(cpu); cpu->PC = addr; cpu->WZ = addr; return 10; }   // JP nn
        case 0xE9: { cpu->PC = cpu->IY; cpu->WZ = cpu->IY; return 4; }   // JP (IY) — substitue JP(HL)
        case 0x18: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); cpu->PC += d; cpu->WZ = cpu->PC; return 12; }   // JR d
        case 0x20: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR NZ,d
        case 0x28: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR Z,d
        case 0x30: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR NC,d
        case 0x38: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR C,d
        case 0xCD: { uint16_t addr = z80_fetch_word(cpu); z80_push_word(cpu, cpu->PC); cpu->PC = addr; cpu->WZ = addr; return 17; }   // CALL nn
        case 0xC9: { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 10; }   // RET
        case 0xC0: { if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET NZ
        case 0xC8: { if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET Z
        case 0xD0: { if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET NC
        case 0xD8: { if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET C

        // ==================== EX / Flags / Interrupts / HALT ====================
        case 0xE3: { /* EX (SP),IY */
            uint16_t val = cpu->IY;
            uint8_t lo = cpu->mem_read_fn(cpu->SP);
            uint8_t hi = cpu->mem_read_fn(cpu->SP + 1);
            cpu->IY = static_cast<uint16_t>((hi << 8) | lo);
            cpu->mem_write_fn(cpu->SP, static_cast<uint8_t>(val & 0xFF));
            cpu->mem_write_fn(cpu->SP + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
            cpu->WZ = cpu->IY;
            return 19; }
        case 0x27: { /* DAA */
            uint8_t a = cpu->A;
            uint8_t correction = 0;
            bool new_carry = false;
            if (TEST_FLAG(cpu, FLAG_H) || (!TEST_FLAG(cpu, FLAG_N) && (a & 0xF) > 9))
                correction |= 0x06;
            if (TEST_FLAG(cpu, FLAG_C) || (!TEST_FLAG(cpu, FLAG_N) && a > 0x99)) {
                correction |= 0x60;
                new_carry = true;
            }
            a += TEST_FLAG(cpu, FLAG_N) ? -static_cast<int8_t>(correction) : static_cast<uint8_t>(correction);
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (a & 0x80)    cpu->F |= FLAG_S;
            if (a == 0)      cpu->F |= FLAG_Z;
            if (new_carry)   cpu->F |= FLAG_C;
            if (z80_parity(a)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, a);
            cpu->A = a;
            return 4; }
        case 0x2F: { /* CPL */
            cpu->A = static_cast<uint8_t>(~cpu->A);
            cpu->F = (cpu->F & 0xC0) | FLAG_H | FLAG_N;
            z80_set_flags_xy(cpu, cpu->A);
            return 4; }
        case 0x37: { /* SCF */
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_N | FLAG_X | FLAG_Y);
            cpu->F |= FLAG_C;
            return 4; }
        case 0x3F: { /* CCF */
            bool co = TEST_FLAG(cpu, FLAG_C);
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y);
            cpu->F |= !co ? FLAG_C : 0;
            return 4; }
        case 0x08: { /* EX AF,AF' */
            uint16_t tmp = cpu->AF;
            cpu->AF = cpu->AF_;
            cpu->AF_ = tmp;
            return 4; }
        case 0xD9: { /* EXX — pas de substitution FD */
            uint16_t tmprp = cpu->BC;
            cpu->BC = cpu->BC_;
            cpu->BC_ = tmprp;
            tmprp = cpu->DE;
            cpu->DE = cpu->DE_;
            cpu->DE_ = tmprp;
            tmprp = cpu->HL;
            cpu->HL = cpu->HL_;
            cpu->HL_ = tmprp;
            return 4; }
        case 0x76: { /* HALT */
            cpu->halted = true;
            cpu->PC--;
            return 4; }

        // ==================== IN/OUT ====================
        case 0xDB: { /* IN A,(n) */
            uint8_t n = z80_fetch_byte(cpu);
            uint16_t port = static_cast<uint16_t>(static_cast<uint16_t>(cpu->A) << 8 | n);
            cpu->A = cpu->io_read_fn(port);
            if (cpu->IM == 2) cpu->WZ = cpu->BC + 1;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (cpu->A & 0x80) cpu->F |= FLAG_S;
            if (cpu->A == 0)   cpu->F |= FLAG_Z;
            z80_set_flags_xy(cpu, cpu->A);
            return 12; }
        case 0xD3: { /* OUT (n),A */
            uint8_t n = z80_fetch_byte(cpu);
            uint16_t port = static_cast<uint16_t>(static_cast<uint16_t>(cpu->A) << 8 | n);
            cpu->io_write_fn(port, cpu->A);
            if (cpu->IM == 2) cpu->WZ = cpu->BC + 1;
            return 12; }

        // ============================================================================
        // Opcodes undocumented IY — ADD IY,r / ADC IY,r / SBC IY,r / AND IYH/IYL
        // OR IYH/IYL / XOR IYH/IYL / CP IYH/IYL (0x80-0xBF)
        // ============================================================================

        case 0x80: { /* ADD IY,B */ return add_iy_rp(cpu, cpu->BC); }   // ADD IY,B = 15
        case 0x81: { /* ADD IY,C */ return add_iy_rp(cpu, cpu->BC); }   // ADD IY,C = 15
        case 0x82: { /* ADD IY,D */ return add_iy_rp(cpu, cpu->DE); }   // ADD IY,D = 15
        case 0x83: { /* ADD IY,E */ return add_iy_rp(cpu, cpu->DE); }   // ADD IY,E = 15
        case 0x84: { /* ADD IY,H */ return add_iy_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFFFF)); } // ADD IY,H = 15
        case 0x85: { /* ADD IY,L */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }         // ADD IY,L = 15
        case 0x86: { /* ADD IY,(IY+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_iy_rp(cpu, val);
        } // ADD IY,(IY+d) = 23
        case 0x87: { /* ADD IY,A */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->A)); }   // ADD IY,A = 15

        // ADC IY,rp undoc
        case 0x88: { /* ADC IY,B */ return add_iy_rp(cpu, cpu->BC); }   // ADC IY,B (undoc) = 15
        case 0x89: { /* ADC IY,C */ return add_iy_rp(cpu, cpu->BC); }   // ADC IY,C (undoc) = 15
        case 0x8A: { /* ADC IY,D */ return add_iy_rp(cpu, cpu->DE); }   // ADC IY,D (undoc) = 15
        case 0x8B: { /* ADC IY,E */ return add_iy_rp(cpu, cpu->DE); }   // ADC IY,E (undoc) = 15
        case 0x8C: { /* ADC IY,H */ return add_iy_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFFFF)); }
        case 0x8D: { /* ADC IY,L */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }
        case 0x8E: { /* ADC IY,(IY+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_iy_rp(cpu, val);
        }
        case 0x8F: { /* ADC IY,A */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->A)); }

        // SBC IY,rp undoc
        case 0x90: { /* SBC IY,B */ return add_iy_rp(cpu, cpu->BC); }   // SBC IY,B (undoc) = 15
        case 0x91: { /* SBC IY,C */ return add_iy_rp(cpu, cpu->BC); }   // SBC IY,C (undoc) = 15
        case 0x92: { /* SBC IY,D */ return add_iy_rp(cpu, cpu->DE); }   // SBC IY,D (undoc) = 15
        case 0x93: { /* SBC IY,E */ return add_iy_rp(cpu, cpu->DE); }   // SBC IY,E (undoc) = 15
        case 0x94: { /* SBC IY,H */ return add_iy_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFFFF)); }
        case 0x95: { /* SBC IY,L */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }
        case 0x96: { /* SBC IY,(IY+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IY) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_iy_rp(cpu, val);
        }
        case 0x97: { /* SBC IY,A */ return add_iy_rp(cpu, static_cast<uint16_t>(cpu->A)); }

        // AND IYH/IYL undoc
        case 0xA4: { /* AND IYH */
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            uint8_t result = cpu->A & val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            cpu->F |= FLAG_H;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xA5: { /* AND IYL */
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            uint8_t result = cpu->A & val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            cpu->F |= FLAG_H;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // OR IYH/IYL undoc
        case 0xB4: { /* OR IYH */
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            uint8_t result = cpu->A | val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xB5: { /* OR IYL */
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            uint8_t result = cpu->A | val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // XOR IYH/IYL undoc
        case 0xAC: { /* XOR IYH */
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            uint8_t result = cpu->A ^ val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xAD: { /* XOR IYL */
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            uint8_t result = cpu->A ^ val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // CP IYH/IYL undoc — XY depuis l'opérande, pas le résultat
        case 0xBE: { /* CP IYH */
            uint8_t val = static_cast<uint8_t>((cpu->IY >> 8) & 0xFF);
            int16_t result = static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val);
            uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
            bool borrow_out = (static_cast<int16_t>(cpu->A) < val);
            bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (res8 == 0)   cpu->F |= FLAG_Z;
            if (res8 & 0x80) cpu->F |= FLAG_S;
            if (borrow_out)  cpu->F |= FLAG_C;
            if (half_borrow) cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, val);   // XY depuis l'OPÉRANDE
            return 4;
        }
        case 0xBF: { /* CP IYL */
            uint8_t val = static_cast<uint8_t>(cpu->IY & 0xFF);
            int16_t result = static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val);
            uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
            bool borrow_out = (static_cast<int16_t>(cpu->A) < val);
            bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (res8 == 0)   cpu->F |= FLAG_Z;
            if (res8 & 0x80) cpu->F |= FLAG_S;
            if (borrow_out)  cpu->F |= FLAG_C;
            if (half_borrow) cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, val);   // XY depuis l'OPÉRANDE
            return 4;
        }

        // ==================== Default: NOP ====================
        default: return 4;
    }
}
