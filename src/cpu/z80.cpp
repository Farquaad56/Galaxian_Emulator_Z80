#include "../include/cpu/z80.h"
#include <iostream>
#include <cstdio>
#include "cpu/z80_ops.h"
#include "cpu/z80_cb.h"
#include "cpu/z80_ed.h"
#include "cpu/z80_dd.h"
#include "cpu/z80_fd.h"
#include "system/z80_bdos.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Core Implementation
// ============================================================================

void z80_init(Z80* cpu) {
    cpu->AF = 0x0000;
    cpu->BC = 0x0000;
    cpu->DE = 0x0000;
    cpu->HL = 0x0000;

    cpu->AF_ = 0x0000;
    cpu->BC_ = 0x0000;
    cpu->DE_ = 0x0000;
    cpu->HL_ = 0x0000;

    cpu->IX = 0x0000;
    cpu->IY = 0x0000;

    cpu->PC = 0x0000;
    cpu->SP = 0xFFFF;

    cpu->I = 0x00;
    cpu->R = 0x00;
    cpu->WZ = 0x0000;

    cpu->IFF1 = false;
    cpu->IFF2 = false;
    cpu->IM = 0;
    cpu->halted = false;
    cpu->ei_delay = false;
    cpu->INT_line = false;
    cpu->NMI_pending = false;

    cpu->mem_read_fn = nullptr;
    cpu->mem_write_fn = nullptr;
    cpu->io_read_fn = nullptr;
    cpu->io_write_fn = nullptr;

    cpu->total_cycles = 0;
}

uint8_t z80_fetch_byte(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->PC);
    cpu->PC++;
    cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
    return val;
}

uint16_t z80_fetch_word(Z80* cpu) {
    uint8_t lo = z80_fetch_byte(cpu);
    uint8_t hi = z80_fetch_byte(cpu);
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint8_t z80_read_r(Z80* cpu, int index) {
    switch (index) {
        case 0: return cpu->B;
        case 1: return cpu->C;
        case 2: return cpu->D;
        case 3: return cpu->E;
        case 4: return cpu->H;
        case 5: return cpu->L;
        case 6: return cpu->mem_read_fn(cpu->HL);
        case 7: return cpu->A;
        default: return 0;
    }
}

void z80_write_r(Z80* cpu, int index, uint8_t value) {
    switch (index) {
        case 0: cpu->B = value; break;
        case 1: cpu->C = value; break;
        case 2: cpu->D = value; break;
        case 3: cpu->E = value; break;
        case 4: cpu->H = value; break;
        case 5: cpu->L = value; break;
        case 6: cpu->mem_write_fn(cpu->HL, value); break;
        case 7: cpu->A = value; break;
        default: break;
    }
}

bool z80_parity(uint8_t val) {
    val ^= val >> 4;
    val ^= val >> 2;
    val ^= val >> 1;
    return !(val & 1);
}

void z80_set_flags_xy(Z80* cpu, uint8_t val) {
    cpu->F &= ~(FLAG_X | FLAG_Y);
    if (val & 0x08) cpu->F |= FLAG_X;
    if (val & 0x20) cpu->F |= FLAG_Y;
}

void z80_set_flags_8bit(Z80* cpu, uint8_t result, bool is_add_sub, bool is_add, bool carry_out, bool half_carry) {
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    if (carry_out) cpu->F |= FLAG_C;
    if (half_carry) cpu->F |= FLAG_H;
    cpu->F |= is_add_sub ? FLAG_N : 0x00;
    if (!is_add_sub && z80_parity(result)) cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, result);
}

void z80_alu_add(Z80* cpu, uint8_t val) {
    uint16_t result = static_cast<uint16_t>(cpu->A) + static_cast<uint16_t>(val);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
    bool carry_out = (result > 0xFF);
    bool half_carry = ((cpu->A & 0xF) + (val & 0xF)) > 0xF;
    bool overflow = (~(static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(val)) &
                     (static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(res8))) & 0x80;

    cpu->F = 0;
    if (res8 == 0)   cpu->F |= FLAG_Z;
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (carry_out)   cpu->F |= FLAG_C;
    if (half_carry)  cpu->F |= FLAG_H;
    if (overflow)    cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, res8);
    cpu->A = res8;
}

void z80_alu_adc(Z80* cpu, uint8_t val) {
    uint8_t carry = TEST_FLAG(cpu, FLAG_C) ? 1 : 0;
    uint16_t result = static_cast<uint16_t>(cpu->A) + static_cast<uint16_t>(val) + static_cast<uint16_t>(carry);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
    bool carry_out = (result > 0xFF);
    bool half_carry = ((cpu->A & 0xF) + (val & 0xF) + carry) > 0xF;
    bool overflow = (~(static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(val)) &
                     (static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(res8))) & 0x80;

    cpu->F = 0;
    if (res8 == 0)   cpu->F |= FLAG_Z;
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (carry_out)   cpu->F |= FLAG_C;
    if (half_carry)  cpu->F |= FLAG_H;
    if (overflow)    cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, res8);
    cpu->A = res8;
}

void z80_alu_sub(Z80* cpu, uint8_t val) {
    int16_t result = static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
    bool borrow_out = (static_cast<int16_t>(cpu->A) < val);
    bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;
    bool overflow = ((static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(val)) &
                     (static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(res8))) & 0x80;

    cpu->F = 0;
    if (res8 == 0)   cpu->F |= FLAG_Z;
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (borrow_out)  cpu->F |= FLAG_C;
    if (half_borrow) cpu->F |= FLAG_H;
    if (overflow)    cpu->F |= FLAG_PV;
    cpu->F |= FLAG_N;
    z80_set_flags_xy(cpu, res8);
    cpu->A = res8;
}

void z80_alu_sbc(Z80* cpu, uint8_t val) {
    uint8_t carry = TEST_FLAG(cpu, FLAG_C) ? 1 : 0;
    int16_t result = static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val) - static_cast<int16_t>(carry);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);
    bool borrow_out = (static_cast<int16_t>(cpu->A) < (val + carry));
    bool half_borrow = ((cpu->A & 0xF) - (val & 0xF) - carry) < 0;
    bool overflow = ((static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(val)) &
                     (static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(res8))) & 0x80;

    cpu->F = 0;
    if (res8 == 0)   cpu->F |= FLAG_Z;
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (borrow_out)  cpu->F |= FLAG_C;
    if (half_borrow) cpu->F |= FLAG_H;
    if (overflow)    cpu->F |= FLAG_PV;
    cpu->F |= FLAG_N;
    z80_set_flags_xy(cpu, res8);
    cpu->A = res8;
}

void z80_alu_and(Z80* cpu, uint8_t val) {
    uint8_t result = cpu->A & val;
    cpu->F = 0;
    if (result == 0)   cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= FLAG_H;
    if (z80_parity(result)) cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, result);
    cpu->A = result;
}

void z80_alu_or(Z80* cpu, uint8_t val) {
    uint8_t result = cpu->A | val;
    cpu->F = 0;
    if (result == 0)   cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    if (z80_parity(result)) cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, result);
    cpu->A = result;
}

void z80_alu_xor(Z80* cpu, uint8_t val) {
    uint8_t result = cpu->A ^ val;
    cpu->F = 0;
    if (result == 0)   cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    if (z80_parity(result)) cpu->F |= FLAG_PV;
    z80_set_flags_xy(cpu, result);
    cpu->A = result;
}

void z80_alu_cp(Z80* cpu, uint8_t val) {
    int16_t result   = static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val);
    uint8_t res8     = static_cast<uint8_t>(result & 0xFF);
    bool borrow_out  = (static_cast<int16_t>(cpu->A) < val);
    bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;
    bool overflow    = ((static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(val)) &
                        (static_cast<int8_t>(cpu->A) ^ static_cast<int8_t>(res8))) & 0x80;

    cpu->F = 0;
    if (res8 == 0)   cpu->F |= FLAG_Z;
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (borrow_out)  cpu->F |= FLAG_C;
    if (half_borrow) cpu->F |= FLAG_H;
    if (overflow)    cpu->F |= FLAG_PV;
    cpu->F |= FLAG_N;
    z80_set_flags_xy(cpu, val);
}

void z80_inc_r(Z80* cpu, int index) {
    uint8_t val    = z80_read_r(cpu, index);
    uint8_t result = static_cast<uint8_t>(val + 1);
    bool half_carry = (val & 0xF) == 0xF;

    cpu->F &= FLAG_C;
    if (result == 0)    cpu->F |= FLAG_Z;
    if (result & 0x80)  cpu->F |= FLAG_S;
    if (half_carry)     cpu->F |= FLAG_H;
    if (val == 0x7F)    cpu->F |= FLAG_PV;

    z80_set_flags_xy(cpu, result);
    z80_write_r(cpu, index, result);
}

void z80_dec_r(Z80* cpu, int index) {
    uint8_t val    = z80_read_r(cpu, index);
    uint8_t result = static_cast<uint8_t>(val - 1);
    bool half_borrow = (val & 0xF) == 0x0;

    cpu->F &= FLAG_C;
    if (result == 0)    cpu->F |= FLAG_Z;
    if (result & 0x80)  cpu->F |= FLAG_S;
    if (half_borrow)    cpu->F |= FLAG_H;
    if (val == 0x80)    cpu->F |= FLAG_PV;

    cpu->F |= FLAG_N;
    z80_set_flags_xy(cpu, result);
    z80_write_r(cpu, index, result);
}

void z80_push_word(Z80* cpu, uint16_t val) {
    cpu->mem_write_fn(--cpu->SP, static_cast<uint8_t>((val >> 8) & 0xFF));
    cpu->mem_write_fn(--cpu->SP, static_cast<uint8_t>(val & 0xFF));
}

uint16_t z80_pop_word(Z80* cpu) {
    uint8_t lo = cpu->mem_read_fn(cpu->SP);
    uint8_t hi = cpu->mem_read_fn(cpu->SP + 1);
    cpu->SP += 2;
    return static_cast<uint16_t>((hi << 8) | lo);
}

void z80_interrupt(Z80* cpu, uint8_t data_bus) {
    if (!cpu->IFF1) return;

    // Sur Z80 réel, IFF1 et IFF2 sont tous deux remis à zéro quand une INT maskable est acceptée.
    cpu->IFF1 = false;
    cpu->IFF2 = false;
    cpu->halted = false;
    z80_push_word(cpu, cpu->PC);

    switch (cpu->IM) {
        case 0:
            // IM 0 : le hardware place RST opcode sur le bus de donnees.
            // Galaxian : hardware place 0xFF (RST 38h) -> saut a 0x0038.
            if (data_bus == 0xFF) {
                cpu->PC = 0x0038;  // RST 38h - handler VBLANK Galaxian
            } else {
                cpu->PC = 0x0000 | data_bus;  // Autres vecteurs RST
            }
            break;
        case 1:
            cpu->PC = 0x0038;
            break;
        case 2: {
            // IM2 : vecteur = (I << 8) | data_bus
            // Le Z80 lit l'octet bas à (I<<8)|data_bus, puis l'octet haut à (I<<8)|data_bus+1
            uint16_t vector_addr = (static_cast<uint16_t>(cpu->I) << 8) | static_cast<uint16_t>(data_bus);
            uint8_t lo = cpu->mem_read_fn(vector_addr);
            uint8_t hi = cpu->mem_read_fn(vector_addr + 1);
            cpu->PC = static_cast<uint16_t>((hi << 8) | lo);
            break;
        }
    }

    cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
}

void z80_nmi(Z80* cpu) {
    cpu->halted = false;
    cpu->IFF2 = cpu->IFF1;
    cpu->IFF1 = false;
    z80_push_word(cpu, cpu->PC);
    cpu->PC = 0x0066;
    cpu->WZ = 0x0066;
    cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
}

int z80_step(Z80* cpu) {
    // NMI : non masquable, prioritaire — prise MÊME pendant la fenêtre EI
    if (cpu->NMI_pending) {
        cpu->NMI_pending = false;
        z80_nmi(cpu);
        return 11;
    }

    // INT maskable : bloquée pendant l'instruction qui suit EI
    bool was_ei_delay = cpu->ei_delay;
    if (!was_ei_delay && cpu->INT_line && cpu->IFF1) {
        cpu->halted = false;
        z80_interrupt(cpu, 0xFF);
        cpu->INT_line = false;
        return (cpu->IM == 2) ? 19 : 13;
    }

    if (was_ei_delay) cpu->ei_delay = false;

    // ====================================================================
    // ÉTAPE 2 : HALT — CPU en pause jusqu'à INT/NMI
    // Si l'opcode qui suit s'avère être un autre EI, il repositionnera
    // ei_delay = true dans l'opcode handler lui-même.
    // ====================================================================
    if (was_ei_delay) {
        cpu->ei_delay = false;
    }

    // ====================================================================
    // ÉTAPE 4 : HALT — CPU en pause jusqu'à INT/NMI
    // ====================================================================
    if (cpu->halted) {
        cpu->R = (cpu->R & 0x80) | ((cpu->R + 1) & 0x7F);
        return 4;
    }

    // ====================================================================
    // ÉTAPE 5 : Fetch + exécution de l'opcode
    // ====================================================================
    uint8_t opcode = z80_fetch_byte(cpu);

    int cycles = 0;
    switch (opcode) {
        case 0xCB: cycles = z80_exec_cb(cpu); break;
        case 0xDD: cycles = z80_exec_dd(cpu); break;
        case 0xED: cycles = z80_exec_ed(cpu); break;
        case 0xFD: cycles = z80_exec_fd(cpu); break;
        default:   cycles = z80_exec_main(cpu, opcode); break;
    }

    // ====================================================================
    // ÉTAPE 6 : APRÈS exécution, activer IFF1/IFF2 si ei_delay était vrai
    // — mais SEULEMENT si l'opcode exécuté n'était pas un autre EI
    // (qui aurait repositionné ei_delay = true).
    // ====================================================================
    if (was_ei_delay && !cpu->ei_delay) {
        cpu->IFF1 = true;
        cpu->IFF2 = true;
    }

    // Enregistrer pc_after dans le traceur d'opcodes
    if (g_opcode_trace_enabled && g_opcode_trace_count > 0) {
        g_opcode_trace[g_opcode_trace_count - 1].pc_after = cpu->PC;
        g_opcode_trace[g_opcode_trace_count - 1].tstates = cycles;
    }

    return cycles;
}

// ============================================================================
// Table de correspondance opcode → mnémonique (fonction statique indépendante)
// ============================================================================
static const char* z80_opcode_mnemonic(uint8_t op) {
    switch(op) {
        case 0x00: return "NOP";
        case 0x01: return "LD BC,n";
        case 0x02: return "LD (BC),A";
        case 0x04: return "INC B";
        case 0x05: return "DEC B";
        case 0x06: return "LD B,n";
        case 0x07: return "RLCA";
        case 0x08: return "EX AF,AF'";
        case 0x09: return "ADD HL,BC";
        case 0x0A: return "LD A,(BC)";
        case 0x0C: return "INC C";
        case 0x0D: return "DEC C";
        case 0x0E: return "LD C,n";
        case 0x0F: return "RRCA";
        case 0x10: return "DJNZ d";
        case 0x11: return "LD DE,n";
        case 0x12: return "LD (DE),A";
        case 0x14: return "INC D";
        case 0x15: return "DEC D";
        case 0x16: return "LD D,n";
        case 0x18: return "JR d";
        case 0x19: return "ADD HL,DE";
        case 0x1A: return "LD A,(DE)";
        case 0x1C: return "INC E";
        case 0x1D: return "DEC E";
        case 0x1E: return "LD E,n";
        case 0x20: return "JR NZ,d";
        case 0x21: return "LD HL,n";
        case 0x22: return "LD (nn),HL";
        case 0x24: return "INC H";
        case 0x25: return "DEC H";
        case 0x26: return "LD H,n";
        case 0x27: return "DAA";
        case 0x28: return "JR Z,d";
        case 0x29: return "ADD HL,HL";
        case 0x2A: return "LD HL,(nn)";
        case 0x2C: return "INC L";
        case 0x2D: return "DEC L";
        case 0x2E: return "LD L,n";
        case 0x30: return "JR NC,d";
        case 0x31: return "LD SP,n";
        case 0x32: return "LD (nn),A";
        case 0x33: return "INC SP";
        case 0x34: return "INC (HL)";
        case 0x35: return "DEC (HL)";
        case 0x36: return "LD (HL),n";
        case 0x37: return "SCF";
        case 0x38: return "JR C,d";
        case 0x39: return "ADD HL,SP";
        case 0x3A: return "LD A,(nn)";
        case 0x3C: return "INC A";
        case 0x3D: return "DEC A";
        case 0x3E: return "LD A,n";
        case 0x40: return "LD B,B";
        case 0x41: return "LD B,C";
        case 0x42: return "LD B,D";
        case 0x43: return "LD B,E";
        case 0x44: return "LD B,H";
        case 0x45: return "LD B,L";
        case 0x46: return "LD B,(HL)";
        case 0x47: return "LD B,A";
        case 0x48: return "LD C,B";
        case 0x49: return "LD C,C";
        case 0x50: return "LD D,B";
        case 0x51: return "LD D,C";
        case 0x52: return "LD D,D";
        case 0x53: return "LD D,E";
        case 0x54: return "LD D,H";
        case 0x55: return "LD D,L";
        case 0x56: return "LD D,(HL)";
        case 0x57: return "LD D,A";
        case 0x58: return "LD E,B";
        case 0x59: return "LD E,C";
        case 0x5A: return "LD E,D";
        case 0x5B: return "LD E,E";
        case 0x60: return "LD H,B";
        case 0x61: return "LD H,C";
        case 0x62: return "LD H,D";
        case 0x63: return "LD H,E";
        case 0x64: return "LD H,H";
        case 0x65: return "LD H,L";
        case 0x66: return "LD H,(HL)";
        case 0x67: return "LD H,A";
        case 0x68: return "LD L,B";
        case 0x69: return "LD L,C";
        case 0x6A: return "LD L,D";
        case 0x6B: return "LD L,E";
        case 0x6C: return "LD L,H";
        case 0x6D: return "LD L,L";
        case 0x6E: return "LD L,(HL)";
        case 0x6F: return "LD L,A";
        case 0x70: return "LD (HL),B";
        case 0x71: return "LD (HL),C";
        case 0x72: return "LD (HL),D";
        case 0x73: return "LD (HL),E";
        case 0x74: return "LD (HL),H";
        case 0x75: return "LD (HL),L";
        case 0x76: return "HALT";
        case 0x77: return "LD (HL),A";
        case 0x78: return "LD A,B";
        case 0x79: return "LD A,C";
        case 0x7A: return "LD A,D";
        case 0x7B: return "LD A,E";
        case 0x7C: return "LD A,H";
        case 0x7D: return "LD A,L";
        case 0x7E: return "LD A,(HL)";
        case 0x7F: return "LD A,A";
        case 0x80: return "ADD A,B";
        case 0x81: return "ADD A,C";
        case 0x82: return "ADD A,D";
        case 0x83: return "ADD A,E";
        case 0x84: return "ADD A,H";
        case 0x85: return "ADD A,L";
        case 0x86: return "ADD A,(HL)";
        case 0x87: return "ADD A,A";
        case 0x90: return "SUB B";
        case 0x91: return "SUB C";
        case 0x92: return "SUB D";
        case 0x93: return "SUB E";
        case 0x94: return "SUB H";
        case 0x95: return "SUB L";
        case 0x96: return "SUB (HL)";
        case 0x97: return "SUB A";
        case 0xA0: return "AND B";
        case 0xA1: return "AND C";
        case 0xA2: return "AND D";
        case 0xA3: return "AND E";
        case 0xA4: return "AND H";
        case 0xA5: return "AND L";
        case 0xA6: return "AND (HL)";
        case 0xA7: return "AND A";
        case 0xB0: return "OR B";
        case 0xB1: return "OR C";
        case 0xB2: return "OR D";
        case 0xB3: return "OR E";
        case 0xB4: return "OR H";
        case 0xB5: return "OR L";
        case 0xB6: return "OR (HL)";
        case 0xB7: return "OR A";
        case 0xC2: return "JP NZ,n";
        case 0xC3: return "JP n";
        case 0xC4: return "CALL NZ,n";
        case 0xC5: return "PUSH BC";
        case 0xC6: return "ADD A,n";
        case 0xC7: return "RST 00";
        case 0xC9: return "RET";
        case 0xCA: return "JP Z,n";
        case 0xCC: return "CALL Z,n";
        case 0xCD: return "CALL n";
        case 0xD2: return "JP NC,n";
        case 0xD3: return "OUT (n),A";
        case 0xD5: return "PUSH DE";
        case 0xD6: return "SUB n";
        case 0xDA: return "JP C,n";
        case 0xDB: return "IN A,(n)";
        case 0xDD: return "PREFIX DD";
        case 0xE2: return "JP PO,n";
        case 0xE3: return "EX (SP),HL";
        case 0xE5: return "PUSH HL";
        case 0xE6: return "AND n";
        case 0xEA: return "JP PE,n";
        case 0xEE: return "XOR n";
        case 0xEF: return "RST 28";
        case 0xF2: return "JP P,n";
        case 0xF3: return "DI";
        case 0xF5: return "PUSH AF";
        case 0xFA: return "JP M,n";
        case 0xFB: return "EI";
        case 0xF9: return "LD SP,HL";
        default:   return "???";
    }
}

// ============================================================================
// Fonction d'impression du traceur d'opcodes — à appeler au moment du timeout
// ============================================================================
void z80_print_opcode_trace(uint16_t max_entries) {
    if (g_opcode_trace_count == 0) {
        printf("[TRACE] Aucun opcode tracé.\n");
        return;
    }

    int count = g_opcode_trace_count < static_cast<int>(max_entries) ? g_opcode_trace_count : static_cast<int>(max_entries);
    printf("\n[TRACE OPCODES] === %d entrées tracées ===\n", count);
    printf("[TRACE] %-6s  %-6s  %-6s  %-10s  %s\n", "PC_BEF", "OP", "PC_AF", "TSTATES", "MNEMONIC");

    for (int i = 0; i < count; i++) {
        OpcodeTraceEntry& e = g_opcode_trace[i];
        printf("[TRACE] %04X  %02X  %04X  %4d    %s\n",
            e.pc_before, e.opcode, e.pc_after, e.tstates,
            z80_opcode_mnemonic(e.opcode));
    }

    // Résumé : compter les opcodes uniques
    printf("\n[TRACE] === RÉSUMÉ des opcodes ===\n");
    static int opcode_counts[256];
    for (int i = 0; i < count; i++) {
        opcode_counts[g_opcode_trace[i].opcode]++;
    }
    for (int op = 0; op < 256; op++) {
        if (opcode_counts[op] > 0) {
            printf("[TRACE] OP=%02X (%-8s) x%d\n", op, z80_opcode_mnemonic(op), opcode_counts[op]);
            opcode_counts[op] = 0; // reset pour prochain appel
        }
    }

    printf("\n[TRACE] === Fin du traceur ===\n");
}