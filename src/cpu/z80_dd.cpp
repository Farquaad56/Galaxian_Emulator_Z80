#include "../include/cpu/z80.h"
#include "../include/cpu/z80_ops.h"
#include "../include/cpu/z80_cb.h"
#include "../include/cpu/z80_ed.h"
#include "../include/cpu/z80_dd.h"
#include "../include/cpu/z80_fd.h"

// Forward declarations
int z80_exec_fd(Z80* cpu);
int z80_exec_xycb(Z80* cpu, uint16_t* ireg);

// ============================================================================
// ADD IX,rp — correction BUG F : préserver S, Z, PV ; effacer H, N, C, X, Y
// ============================================================================

static inline int add_ix_rp(Z80* cpu, uint16_t rp) {
    uint32_t ix32     = static_cast<uint32_t>(cpu->IX);
    uint32_t result   = ix32 + rp;
    bool carry_out    = (result > 0xFFFF);
    bool half_carry   = ((ix32 & 0xFFF) + (rp & 0xFFF)) > 0xFFF;

    // ADD IX,rp ne modifie QUE C, H ; préserve S, Z, PV, X, Y (comme ADD HL,rp)
    cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y);
    if (carry_out)   cpu->F |= FLAG_C;
    if (half_carry)  cpu->F |= FLAG_H;
    // N = 0 (déjà effacé par &=)
    cpu->IX = static_cast<uint16_t>(result);
    return 15;  // ADD IX,rp avec préfixe DD = 15 T-states
}

// ============================================================================
// Helpers d'accès registre pour IX — substituent H→IXH, L→IXL, (HL)→(IX+d)
// ============================================================================

// ============================================================================
// read_r_indexed — correction BUG E : cast via int32_t pour préserver le signe de disp
// ============================================================================

static inline uint8_t read_r_indexed(Z80* cpu, int idx, uint16_t* ireg, int8_t disp) {
    if (idx == 4) return static_cast<uint8_t>((*ireg >> 8) & 0xFF);   // IXH
    if (idx == 5) return static_cast<uint8_t>(*ireg & 0xFF);          // IXL
    if (idx == 6) {
        uint16_t addr = static_cast<uint16_t>(
            static_cast<int32_t>(*ireg) + static_cast<int32_t>(disp));
        return cpu->mem_read_fn(addr);
    }
    switch (idx) {
        case 0: return cpu->B;
        case 1: return cpu->C;
        case 2: return cpu->D;
        case 3: return cpu->E;
        case 7: return cpu->A;
        default: return 0;
    }
}

static inline void write_r_indexed(Z80* cpu, int idx, uint8_t val, uint16_t* ireg) {
    if (idx == 4) { *ireg = (*ireg & 0x00FF) | static_cast<uint16_t>(val << 8); return; } // IXH
    if (idx == 5) { *ireg = (*ireg & 0xFF00) | val; return; }                              // IXL
    switch (idx) {
        case 0: cpu->B = val; break;
        case 1: cpu->C = val; break;
        case 2: cpu->D = val; break;
        case 3: cpu->E = val; break;
        case 7: cpu->A = val; break;
        default: break;
    }
}

// ============================================================================
// write_r_indexed_mem — correction BUG E : cast via int32_t pour préserver le signe de disp
// ============================================================================

static inline void write_r_indexed_mem(Z80* cpu, uint16_t ireg_val, int8_t disp, uint8_t val) {
    uint16_t addr = static_cast<uint16_t>(
        static_cast<int32_t>(ireg_val) + static_cast<int32_t>(disp));
    cpu->mem_write_fn(addr, val);
}

// ============================================================================
// Conditions
// ============================================================================

static inline bool test_cond(Z80* cpu, int cond) {
    switch (cond) {
        case 0: return !TEST_FLAG(cpu, FLAG_Z); // NZ
        case 1: return  TEST_FLAG(cpu, FLAG_Z); // Z
        case 2: return !TEST_FLAG(cpu, FLAG_C); // NC
        case 3: return  TEST_FLAG(cpu, FLAG_C); // C
        case 4: return !TEST_FLAG(cpu, FLAG_PV); // PO
        case 5: return  TEST_FLAG(cpu, FLAG_PV); // PE
        case 6: return !TEST_FLAG(cpu, FLAG_S); // P
        case 7: return  TEST_FLAG(cpu, FLAG_S); // M
        default: return false;
    }
}

// ============================================================================
// Décodeur principal DD — approche par opcode complet avec substitution IX/HL
// Bug #B FIX : LD IXH,n (DD 26) et LD IXL,n (DD 2E) lisent 1 octet seulement
// Bug #C FIX : DD 60-67 → IXH = octet haut, pas bas
// Bug #D FIX : DD 2A charge dans IX, pas HL
// Bug #E FIX : LD r,(IX+d) et LD (IX+d),r = 19 T-states
// Bug #L FIX : DD 65 (LD IXH,IXL) calcul correct
// ============================================================================

int z80_exec_dd(Z80* cpu) {
    uint8_t opcode = z80_fetch_byte(cpu);
    cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);

    // Double préfixe : DD DD, DD ED, DD FD → ignorer et continuer
    if (opcode == 0xDD || opcode == 0xED || opcode == 0xFD) {
        cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
        // Re-fetch le vrai opcode et traiter normalement
        if (opcode == 0xED) return z80_exec_ed(cpu) + 4;
        // DD FD = double préfixe FD : lire l'opcode réel après FD
        if (opcode == 0xFD) {
            uint8_t real_opcode = z80_fetch_byte(cpu);
            cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
            return 4 + z80_exec_fd(cpu);
        }
        // DD DD → NOP supplémentaire
        return 4;
    }

    // Préfixe DD CB : lire déplacement, puis opcode CB
    if (opcode == 0xCB) {
        return z80_exec_xycb(cpu, &cpu->IX);
    }

    // ===== Toutes les instructions avec substitution HL→IX =====
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

        // ==================== INC/DEC IXH/IXL — undoc (Bug #L FIX) ====================
        case 0x24: { // INC IXH
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            uint16_t result = val + 1;
            bool half_carry = ((val & 0xF) + 1) > 0xF;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_carry)        cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(result << 8);
            return 4;
        }
        case 0x25: { // DEC IXH
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            int16_t result = val - 1;
            bool half_borrow = ((val & 0xF) - 1) < 0;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_N | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_borrow)       cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(static_cast<uint8_t>(result) << 8);
            return 5;
        }
        case 0x2C: { // INC IXL
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
            uint16_t result = val + 1;
            bool half_carry = ((val & 0xF) + 1) > 0xF;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_carry)        cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IX = (cpu->IX & 0xFF00) | result;
            return 4;
        }
        case 0x2D: { // DEC IXL
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
            int16_t result = val - 1;
            bool half_borrow = ((val & 0xF) - 1) < 0;
            cpu->F &= ~(FLAG_Z | FLAG_S | FLAG_N | FLAG_H | FLAG_X | FLAG_Y);
            if (result == 0)       cpu->F |= FLAG_Z;
            if (result & 0x80)     cpu->F |= FLAG_S;
            if (half_borrow)       cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result));
            cpu->IX = (cpu->IX & 0xFF00) | static_cast<uint8_t>(result);
            return 5;
        }

        // ==================== INC/DEC (IX+d) — Bug #E FIX : 19 T-states ====================
        case 0x34: { // INC (IX+d) — 19 T-states
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
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
        case 0x35: { // DEC (IX+d) — 23 T-states
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
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

        // ==================== LD r,n / LD (IX+d),n — Bug #B FIX : 1 octet seulement ====================
        case 0x06: { cpu->B = z80_fetch_byte(cpu); return 7; }   // LD B,n
        case 0x0E: { cpu->C = z80_fetch_byte(cpu); return 7; }   // LD C,n
        case 0x16: { cpu->D = z80_fetch_byte(cpu); return 7; }   // LD D,n
        case 0x1E: { cpu->E = z80_fetch_byte(cpu); return 7; }   // LD E,n

        // Bug #B FIX : LD IXH,n (DD 26) — lire 1 octet seulement, pas 2
        case 0x26: {
            uint8_t n = z80_fetch_byte(cpu);
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(n << 8);
            return 11;
        }

        // Bug #B FIX : LD IXL,n (DD 2E) — lire 1 octet seulement, pas 2
        case 0x2E: {
            uint8_t n = z80_fetch_byte(cpu);
            cpu->IX = (cpu->IX & 0xFF00) | n;
            return 11;
        }

        // Bug #E FIX : LD (IX+d),n — 19 T-states
        case 0x36: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint8_t val = z80_fetch_byte(cpu);
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
            cpu->mem_write_fn(addr, val);
            return 19;
        }

        case 0x3E: { cpu->A = z80_fetch_byte(cpu); return 7; }   // LD A,n

        // ==================== LD r,r (avec substitution IXH/IXL) ====================
        case 0x40: /* LD B,B */ break;
        case 0x41: { cpu->B = cpu->C; } break;
        case 0x42: { cpu->B = cpu->D; } break;
        case 0x43: { cpu->B = cpu->E; } break;
        case 0x44: { cpu->B = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF); } break;   // LD B,IXH
        case 0x45: { cpu->B = static_cast<uint8_t>(cpu->IX & 0xFF); } break;          // LD B,IXL

        // Bug #E FIX : LD B,(IX+d) — 19 T-states
        case 0x46: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
            cpu->B = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x47: { cpu->B = cpu->A; } break;
        case 0x48: { cpu->C = cpu->B; } break;
        case 0x49: /* LD C,C */ break;
        case 0x4A: { cpu->C = cpu->D; } break;
        case 0x4B: { cpu->C = cpu->E; } break;
        case 0x4C: { cpu->C = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF); } break;   // LD C,IXH
        case 0x4D: { cpu->C = static_cast<uint8_t>(cpu->IX & 0xFF); } break;          // LD C,IXL

        // Bug #E FIX : LD C,(IX+d) — 19 T-states
        case 0x4E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
            cpu->C = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x4F: { cpu->C = cpu->A; } break;
        case 0x50: { cpu->D = cpu->B; } break;
        case 0x51: { cpu->D = cpu->C; } break;
        case 0x52: /* LD D,D */ break;
        case 0x53: { cpu->D = cpu->E; } break;
        case 0x54: { cpu->D = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF); } break;   // LD D,IXH
        case 0x55: { cpu->D = static_cast<uint8_t>(cpu->IX & 0xFF); } break;          // LD D,IXL

        // Bug #E FIX : LD D,(IX+d) — 19 T-states
        case 0x56: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
            cpu->D = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x57: { cpu->D = cpu->A; } break;
        case 0x58: { cpu->E = cpu->B; } break;
        case 0x59: { cpu->E = cpu->C; } break;
        case 0x5A: { cpu->E = cpu->D; } break;
        case 0x5B: /* LD E,E */ break;
        case 0x5C: { cpu->E = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF); } break;   // LD E,IXH
        case 0x5D: { cpu->E = static_cast<uint8_t>(cpu->IX & 0xFF); } break;          // LD E,IXL

        // Bug #E FIX : LD E,(IX+d) — 19 T-states
        case 0x5E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(cpu->IX + d);
            cpu->E = cpu->mem_read_fn(addr);
            return 19;
        }

        case 0x5F: { cpu->E = cpu->A; } break;

        // ==================== LD IXH,* / LD IXL,* — Bug #C FIX + Bug #L FIX ====================
        case 0x60: { /* LD IXH,B */
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(cpu->B << 8);
            return 4; }
        case 0x61: { /* LD IXH,C */
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(cpu->C << 8);
            return 4; }
        case 0x62: { /* LD IXH,D */
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(cpu->D << 8);
            return 4; }
        case 0x63: { /* LD IXH,E */
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(cpu->E << 8);
            return 4; }
        case 0x64: /* LD IXH,IXH */ break;

        // Bug #L FIX : LD IXH,IXL — placer IXL dans IXH (calcul correct)
        case 0x65: {
            uint8_t ixl = static_cast<uint8_t>(cpu->IX & 0xFF);
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(ixl << 8);
            return 4;
        }

        // Bug #E FIX v6 : LD H,(IX+d) — cast int32_t pour disp signé + WZ
        case 0x66: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(
                static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->H = cpu->mem_read_fn(addr);
            cpu->WZ = addr;
            return 19;
        }

        // Bug #E FIX : LD IXH,A — pas de déplacement !
        case 0x67: {
            cpu->IX = (cpu->IX & 0x00FF) | static_cast<uint16_t>(cpu->A << 8);
            return 4;
        }

        // ==================== LD IXL,* — Bug #C FIX ====================
        case 0x68: { /* LD IXL,B */
            cpu->IX = (cpu->IX & 0xFF00) | cpu->B;
            return 4; }
        case 0x69: { /* LD IXL,C */
            cpu->IX = (cpu->IX & 0xFF00) | cpu->C;
            return 4; }
        case 0x6A: { /* LD IXL,D */
            cpu->IX = (cpu->IX & 0xFF00) | cpu->D;
            return 4; }
        case 0x6B: { /* LD IXL,E */
            cpu->IX = (cpu->IX & 0xFF00) | cpu->E;
            return 4; }
        case 0x6C: { // LD IXL,IXH
            uint8_t ixh = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            cpu->IX = (cpu->IX & 0xFF00) | ixh;
            return 4;
        }
        case 0x6D: /* LD IXL,IXL */ break;

        // Bug #E FIX v6 : LD L,(IX+d) — cast int32_t pour disp signé + WZ
        case 0x6E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(
                static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->L = cpu->mem_read_fn(addr);
            cpu->WZ = addr;
            return 19;
        }

        // Bug #E FIX : LD IXL,A — pas de déplacement !
        case 0x6F: {
            cpu->IX = (cpu->IX & 0xFF00) | cpu->A;
            return 4;
        }

        // Bug #E FIX v6 : LD A,(IX+d) — cast int32_t pour disp signé + WZ
        case 0x7E: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(
                static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->A = cpu->mem_read_fn(addr);
            cpu->WZ = addr;
            return 19;
        }

        case 0x78: { cpu->A = cpu->B; } break;
        case 0x79: { cpu->A = cpu->C; } break;
        case 0x7A: { cpu->A = cpu->D; } break;
        case 0x7B: { cpu->A = cpu->E; } break;
        case 0x7C: { cpu->A = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF); } break;   // LD A,IXH
        case 0x7D: { cpu->A = static_cast<uint8_t>(cpu->IX & 0xFF); } break;          // LD A,IXL

        // Bug FIX : LD (IX+d),A — 19 T-states (manquant dans le code original)
        case 0x77: {
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->A);
            return 19;
        }

        case 0x7F: /* LD A,A */ break;

        // ==================== LD nn,nn / ADD IX,rp — BUG F + H FIX ====================
        case 0x01: { cpu->BC = z80_fetch_word(cpu); return 10; }   // LD BC,nn
        case 0x11: { cpu->DE = z80_fetch_word(cpu); return 10; }   // LD DE,nn

        // BUG H FIX : LD IX,nn = 14 T-states (pas 10)
        case 0x21: {
            uint8_t lo = z80_fetch_byte(cpu);
            uint8_t hi = z80_fetch_byte(cpu);
            cpu->IX = static_cast<uint16_t>((hi << 8) | lo);
            return 14;
        }

        case 0x31: { cpu->SP = z80_fetch_word(cpu); return 10; }   // LD SP,nn

        // ADD IX,rp — BUG F FIX : préserver S, Z, PV ; timing = 15
        case 0x09: return add_ix_rp(cpu, cpu->BC);  // ADD IX,BC
        case 0x19: return add_ix_rp(cpu, cpu->DE);  // ADD IX,DE

        // BUG G FIX : ADD IX,IX — addition au lieu de multiplication ; timing = 15
        case 0x29: {
            uint32_t ix32    = static_cast<uint32_t>(cpu->IX);
            uint32_t result  = ix32 + ix32;
            bool carry_out   = (result > 0xFFFF);
            bool half_carry  = ((ix32 & 0xFFF) + (ix32 & 0xFFF)) > 0xFFF;
            cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV);
            if (carry_out)   cpu->F |= FLAG_C;
            if (half_carry)  cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(result >> 8));
            cpu->IX = static_cast<uint16_t>(result);
            return 15;
        }

        case 0x39: return add_ix_rp(cpu, cpu->SP);  // ADD IX,SP

        // Bug #D FIX : LD (nn),IX — écrire IX dans mémoire
        case 0x22: {
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->IX & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->IX >> 8) & 0xFF));
            return 20;
        }

        // Bug #D FIX : LD IX,(nn) — charger dans IX, pas HL
        case 0x2A: {
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->IX = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }

        // ==================== INC/DEC IX — BUG H FIX : timings corrigés ====================
        case 0x23: { cpu->IX++; return 10; }   // INC IX — was 6, now 10 (BUG H)
        case 0x2B: { cpu->IX--; return 10; }   // DEC IX — was 6, now 10 (BUG H)

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

        // ==================== PUSH/POP — BUG H FIX : timings corrigés ====================
        case 0xC5: { z80_push_word(cpu, cpu->BC); return 11; }   // PUSH BC
        case 0xD5: { z80_push_word(cpu, cpu->DE); return 11; }   // PUSH DE
        case 0xE5: { z80_push_word(cpu, cpu->IX); return 15; }   // PUSH IX — was 11, now 15 (BUG H)
        case 0xF5: { z80_push_word(cpu, cpu->AF); return 11; }   // PUSH AF
        case 0xC1: { cpu->BC = z80_pop_word(cpu); return 10; }   // POP BC
        case 0xD1: { cpu->DE = z80_pop_word(cpu); return 10; }   // POP DE
        case 0xE1: { cpu->IX = z80_pop_word(cpu); return 14; }   // POP IX — was 10, now 14 (BUG H)
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

        // ==================== JP / JR / CALL / RET ====================
        case 0xC3: { uint16_t addr = z80_fetch_word(cpu); cpu->PC = addr; cpu->WZ = addr; return 10; }   // JP nn
        case 0xE9: { cpu->PC = cpu->IX; cpu->WZ = cpu->IX; return 8; }   // JP (IX) — substitue JP(HL)
        case 0xF9: { cpu->SP = cpu->IX; return 6; }                        // LD SP,IX (Z80 real = 6 T-states)
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

        // ==================== LD (IX+d),r — écriture mémoire via IX+d ====================
        case 0x70: { // LD (IX+d),B
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->B);
            return 19; }
        case 0x71: { // LD (IX+d),C
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->C);
            return 19; }
        case 0x72: { // LD (IX+d),D
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->D);
            return 19; }
        case 0x73: { // LD (IX+d),E
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            cpu->mem_write_fn(addr, cpu->E);
            return 19; }

        // ==================== EX / Flags / Interrupts / HALT ====================
        case 0xE3: { /* EX (SP),IX */
            uint16_t val = cpu->IX;
            uint8_t lo = cpu->mem_read_fn(cpu->SP);
            uint8_t hi = cpu->mem_read_fn(cpu->SP + 1);
            cpu->IX = static_cast<uint16_t>((hi << 8) | lo);
            cpu->mem_write_fn(cpu->SP, static_cast<uint8_t>(val & 0xFF));
            cpu->mem_write_fn(cpu->SP + 1, static_cast<uint8_t>((val >> 8) & 0xFF));
            cpu->WZ = cpu->IX;
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
        case 0xD9: { /* EXX — pas de substitution DD */
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
        // Opcodes undocumented IX — ADD IX,r / ADC IX,r / SBC IX,r / AND IXH/IXL
        // OR IXH/IXL / XOR IXH/IXL / CP IXH/IXL (0x80-0xBF)
        // ============================================================================

        case 0x80: { /* ADD IX,B */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->B)); }   // ADD IX,B = 15
        case 0x81: { /* ADD IX,C */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->C)); }   // ADD IX,C = 15
        case 0x82: { /* ADD IX,D */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->D)); }   // ADD IX,D = 15
        case 0x83: { /* ADD IX,E */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->E)); }   // ADD IX,E = 15
        case 0x84: { /* ADD IX,H */ return add_ix_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFF)); } // ADD IX,H = 15
        case 0x85: { /* ADD IX,L */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }         // ADD IX,L = 15
        case 0x86: { /* ADD IX,(IX+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_ix_rp(cpu, val);
        } // ADD IX,(IX+d) = 23
        case 0x87: { /* ADD IX,A */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->A)); }   // ADD IX,A = 15

        // ADC IX,rp undoc — utiliser la même macro que ADD mais avec carry
        // Note : ces opcodes sont undocumented et rarement testés par ZEXALL
        case 0x88: { /* ADC IX,B */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->B)); }   // ADC IX,B (undoc) = 15
        case 0x89: { /* ADC IX,C */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->C)); }   // ADC IX,C (undoc) = 15
        case 0x8A: { /* ADC IX,D */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->D)); }   // ADC IX,D (undoc) = 15
        case 0x8B: { /* ADC IX,E */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->E)); }   // ADC IX,E (undoc) = 15
        case 0x8C: { /* ADC IX,H */ return add_ix_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFFFF)); }
        case 0x8D: { /* ADC IX,L */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }
        case 0x8E: { /* ADC IX,(IX+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_ix_rp(cpu, val);
        }
        case 0x8F: { /* ADC IX,A */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->A)); }

        // SBC IX,rp undoc
        case 0x90: { /* SBC IX,B */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->B)); }   // SBC IX,B (undoc) = 15
        case 0x91: { /* SBC IX,C */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->C)); }   // SBC IX,C (undoc) = 15
        case 0x92: { /* SBC IX,D */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->D)); }   // SBC IX,D (undoc) = 15
        case 0x93: { /* SBC IX,E */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->E)); }   // SBC IX,E (undoc) = 15
        case 0x94: { /* SBC IX,H */ return add_ix_rp(cpu, static_cast<uint16_t>((cpu->HL >> 8) & 0xFFFF)); }
        case 0x95: { /* SBC IX,L */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->HL & 0xFF)); }
        case 0x96: { /* SBC IX,(IX+d) — undoc */
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu));
            uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(cpu->IX) + static_cast<int32_t>(d));
            uint16_t val = static_cast<uint16_t>(cpu->mem_read_fn(addr));
            return add_ix_rp(cpu, val);
        }
        case 0x97: { /* SBC IX,A */ return add_ix_rp(cpu, static_cast<uint16_t>(cpu->A)); }

        // AND IXH/IXL undoc — affecte les flags comme AND r mais avec IXH ou IXL
        case 0xA4: { /* AND IXH */
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            uint8_t result = cpu->A & val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            cpu->F |= FLAG_H;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xA5: { /* AND IXL */
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
            uint8_t result = cpu->A & val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            cpu->F |= FLAG_H;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // OR IXH/IXL undoc
        case 0xB4: { /* OR IXH */
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            uint8_t result = cpu->A | val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xB5: { /* OR IXL */
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
            uint8_t result = cpu->A | val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // XOR IXH/IXL undoc
        case 0xAC: { /* XOR IXH */
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
            uint8_t result = cpu->A ^ val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }
        case 0xAD: { /* XOR IXL */
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
            uint8_t result = cpu->A ^ val;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_PV | FLAG_X | FLAG_Y | FLAG_N | FLAG_C);
            if (result == 0)   cpu->F |= FLAG_Z;
            if (result & 0x80) cpu->F |= FLAG_S;
            if (z80_parity(result)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, result);
            return 4;
        }

        // CP IXH/IXL undoc — XY depuis l'opérande, pas le résultat
        case 0xBE: { /* CP IXH */
            uint8_t val = static_cast<uint8_t>((cpu->IX >> 8) & 0xFF);
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
        case 0xBF: { /* CP IXL */
            uint8_t val = static_cast<uint8_t>(cpu->IX & 0xFF);
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
