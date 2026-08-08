#include "../include/cpu/z80.h"
#include "../include/cpu/z80_ops.h"
#include "../include/cpu/z80_cb.h"
#include "../include/cpu/z80_ed.h"
#include <cstdio>


// ============================================================================
// ADC/SBC HL,rp — opcodes ED 4A/5A/6A/7A (ADC) et ED 42/52/62/72 (SBC)
// Bug #G FIX : Utiliser uint32_t pour détecter le carry, XY sur octet HAUT
// ============================================================================

// ============================================================================
// ADC/SBC HL,rp — Bug FIX rapport v7 : flags Z/PV/X/Y corrects pour ZEXALL
// ============================================================================

static int adc_hl_rp(Z80* cpu, uint16_t rp) {
    uint32_t hl = static_cast<uint32_t>(cpu->HL);
    uint32_t val = static_cast<uint32_t>(rp);
    uint32_t carry_in = (cpu->F & FLAG_C) ? 1 : 0;
    uint32_t res = hl + val + carry_in;

    cpu->F = 0;
    uint16_t res16 = static_cast<uint16_t>(res & 0xFFFF);
    
    // Flags de base
    if (res16 == 0)      cpu->F |= FLAG_Z;
    if (res16 & 0x8000)  cpu->F |= FLAG_S;
    if (res & 0x10000)   cpu->F |= FLAG_C;
    
    // Half-carry (H) : retenue du bit 11 vers le bit 12
    if (((hl & 0xFFF) + (val & 0xFFF) + carry_in) > 0xFFF) cpu->F |= FLAG_H;
    
    // Overflow (V/PV) : pour ADD/ADC, overflow si les signes des opérandes sont identiques
    // mais le signe du résultat est différent — formule correcte Z80
    if ((~(hl ^ val) & (hl ^ res)) & 0x8000) cpu->F |= FLAG_PV;

    // Flags X et Y : copiés depuis l'octet de poids fort du résultat
    cpu->F |= (static_cast<uint8_t>(res16 >> 8) & (FLAG_X | FLAG_Y));

    cpu->HL = res16;
    return 15;
}

static int sbc_hl_rp(Z80* cpu, uint16_t rp) {
    uint32_t hl = static_cast<uint32_t>(cpu->HL);
    uint32_t val = static_cast<uint32_t>(rp);
    uint32_t carry = (cpu->F & FLAG_C) ? 1 : 0;
    uint32_t res = hl - val - carry;

    cpu->F = FLAG_N; // Soustraction
    uint16_t res16 = static_cast<uint16_t>(res & 0xFFFF);

    if (res16 == 0)      cpu->F |= FLAG_Z;
    if (res16 & 0x8000)  cpu->F |= FLAG_S;
    if (res & 0x10000)   cpu->F |= FLAG_C; // Borrow (Emprunt)
    
    // Half-carry (H) : emprunt du bit 12
    if (((hl & 0xFFF) < (val & 0xFFF) + carry)) cpu->F |= FLAG_H;
    
    // Overflow (V)
    if (((hl ^ val) & (hl ^ res)) & 0x8000) cpu->F |= FLAG_PV;

    // Flags X et Y : copiés depuis l'octet de poids fort du résultat
    cpu->F |= (static_cast<uint8_t>(res16 >> 8) & (FLAG_X | FLAG_Y));

    cpu->HL = res16;
    return 15;
}

// ============================================================================
// Block transfer instructions — LDIR, LDDR, CPI, CPIR
// Bug #H FIX : PC rewind quand BC≠0, flags H=0 N=0, PV=(BC≠0)
// ============================================================================

// ============================================================================
// LDI/LDIR/LDD/LDDR — Bug FIX rapport v9 : Flags X/Y depuis (byte_transféré + A)
// Section 2.2.5 du document Z80 undocumented de Sean Young :
// "5 is bit 1 of (transferred byte + A); 3 is bit 3 of (transferred byte + A)"
// ============================================================================

static int ldir(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->HL);
    cpu->mem_write_fn(cpu->DE, val);
    cpu->HL++;
    cpu->DE++;
    cpu->BC--;

    // H = 0, N = 0
    cpu->F &= ~(FLAG_H | FLAG_N | FLAG_PV);
    
    // Bug FIX rapport v9 : X/Y depuis (val + A) avant l'écriture en DE
    uint8_t sum = static_cast<uint8_t>(val + cpu->A);
    cpu->F &= ~(FLAG_X | FLAG_Y);
    if (sum & 0x08) cpu->F |= FLAG_X;
    if (sum & 0x20) cpu->F |= FLAG_Y;

    if (cpu->BC != 0) {
        cpu->F |= FLAG_PV;
        cpu->PC -= 2;  // Ré-exécuter LDIR au prochain step
        return 21;
    }
    return 16;
}

static int lddr(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->HL);
    cpu->mem_write_fn(cpu->DE, val);
    cpu->HL--;
    cpu->DE--;
    cpu->BC--;

    cpu->F &= ~(FLAG_H | FLAG_N | FLAG_PV);
    
    // Bug FIX rapport v9 : X/Y depuis (val + A) avant l'écriture en DE
    uint8_t sum = static_cast<uint8_t>(val + cpu->A);
    cpu->F &= ~(FLAG_X | FLAG_Y);
    if (sum & 0x08) cpu->F |= FLAG_X;
    if (sum & 0x20) cpu->F |= FLAG_Y;

    if (cpu->BC != 0) {
        cpu->F |= FLAG_PV;
        cpu->PC -= 2;
        return 21;
    }
    return 16;
}

// ============================================================================
// CPI/CPIR/CPD/CPDR — Bug FIX rapport v9 : Flags X/Y depuis (A - (HL) - H_flag)
// Section 2.2.5 du document Z80 undocumented de Sean Young :
// "3 is bit 3 of (A - (HL) - H); 5 is bit 1 of (A - (HL) - H)"
// ============================================================================

static int cpi(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->HL);
    uint16_t result = static_cast<uint16_t>(cpu->A) - static_cast<uint16_t>(val);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);

    bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;

    cpu->HL++;
    cpu->BC--;

    // Flags selon le Z80 réel pour CPI
    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
    if (cpu->A == val) cpu->F |= FLAG_Z;  // Z=1 si A==(HL) avant incrément
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (half_borrow) cpu->F |= FLAG_H;
    cpu->F |= FLAG_N;

    // Bug #I FIX : PV = BC≠0, pas parité du résultat
    if (cpu->BC != 0) cpu->F |= FLAG_PV;

    // Bug FIX rapport v9 : X/Y depuis (A - val - H_flag), pas depuis res8
    uint8_t temp = static_cast<uint8_t>(static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val) - (half_borrow ? 1 : 0));
    cpu->F &= ~(FLAG_X | FLAG_Y);
    if (temp & 0x08) cpu->F |= FLAG_X;
    if (temp & 0x20) cpu->F |= FLAG_Y;

    return 16;
}

static int cpir(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->HL);
    uint16_t result = static_cast<uint16_t>(cpu->A) - static_cast<uint16_t>(val);
    uint8_t res8 = static_cast<uint8_t>(result & 0xFF);

    bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;

    cpu->HL++;
    cpu->BC--;

    // Flags selon le Z80 réel pour CPIR
    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
    if (cpu->A == val) cpu->F |= FLAG_Z;  // Z=1 si A==(HL)
    if (res8 & 0x80) cpu->F |= FLAG_S;
    if (half_borrow) cpu->F |= FLAG_H;
    cpu->F |= FLAG_N;

    // Bug #I FIX : PV = BC≠0, pas parité du résultat
    if (cpu->BC != 0) cpu->F |= FLAG_PV;

    // Bug FIX rapport v9 : X/Y depuis (A - val - H_flag), pas depuis res8
    uint8_t temp = static_cast<uint8_t>(static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val) - (half_borrow ? 1 : 0));
    cpu->F &= ~(FLAG_X | FLAG_Y);
    if (temp & 0x08) cpu->F |= FLAG_X;
    if (temp & 0x20) cpu->F |= FLAG_Y;

    // Vérifier si la boucle continue
    bool done = (cpu->BC == 0 || (cpu->A == val));

    if (!done) {
        // Revenir au début de l'instruction CPIR (PC -= 2)
        cpu->PC -= 2;
        return 16 + 21;  // 16 cycles pour CPI + 21 cycles par itération supplémentaire
    }

    // Dernière itération : BC=0 ou valeur trouvée
    return 16 + 5;  // Dernière itération = 21 cycles total
}

// ============================================================================
// Block I/O instructions — INI, OTDR, INDR, INIR
// Bug FIX rapport v10 : Flags N, C/H, P/V selon Sean Young §2.2.6
// ============================================================================

static int ini(Z80* cpu) {
    uint8_t val = cpu->io_read_fn(cpu->BC);
    cpu->mem_write_fn(cpu->HL, val);

    cpu->WZ = cpu->BC + 1;

    // Flags N, C, H selon Sean Young §2.2.6 :
    // N = bit 7 de la valeur I/O
    // C et H = carry de (C+1) + port_value
    uint16_t c_plus_1 = static_cast<uint16_t>(cpu->BC) + 1;
    uint32_t sum_ch = static_cast<uint32_t>(c_plus_1) + val;
    bool carry_ch = (sum_ch > 0xFFFF);

    // P/V flag selon table Temp1/Temp2 de Sean Young §2.2.6
    bool c_bit0 = cpu->BC & 1;
    bool c_bit1 = cpu->BC & 2;
    bool inp_bit0 = val & 1;
    bool inp_bit1 = val & 2;
    bool inp_bit2 = val & 4;
    bool c_bit2 = cpu->BC & 4;

    // Table Temp1 pour INI/OTI
    bool temp1 = ((c_bit1 && !c_bit0 && inp_bit0 && inp_bit1) ||
                  (!c_bit1 && c_bit0 && inp_bit1) ||
                  (c_bit1 && c_bit0 && inp_bit0) ||
                  (c_bit1 && c_bit0 && inp_bit1));

    // Temp2 : Parity(B) xor (B.4 or (B.6 and not B.5)) si low nibble B != 0
    bool parity_b = !((cpu->B >> 0 & 1) ^ (cpu->B >> 1 & 1) ^ (cpu->B >> 2 & 1) ^
                      (cpu->B >> 3 & 1) ^ (cpu->B >> 4 & 1) ^ (cpu->B >> 5 & 1) ^
                      (cpu->B >> 6 & 1) ^ (cpu->B >> 7 & 1));
    bool low_b_nonzero = cpu->B & 0x0F;
    bool temp2;
    if (!low_b_nonzero) {
        temp2 = parity_b ^ ((cpu->B >> 4 & 1) || ((cpu->B >> 6 & 1) && !(cpu->B >> 5 & 1)));
    } else {
        temp2 = parity_b ^ ((cpu->B >> 0 & 1) || ((cpu->B >> 2 & 1) && !(cpu->B >> 1 & 1)));
    }

    bool pv_flag = temp1 ^ temp2 ^ c_bit2 ^ inp_bit2;

    cpu->B--;
    cpu->HL++;

    // Flags complets selon Sean Young §2.2.6
    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C);
    if (cpu->B == 0)   cpu->F |= FLAG_Z;
    if (cpu->B & 0x80) cpu->F |= FLAG_S;
    if (carry_ch)      cpu->F |= FLAG_H | FLAG_C;
    cpu->F |= (val & 0x80) ? FLAG_N : 0;  // N = bit 7 de val
    if (pv_flag)       cpu->F |= FLAG_PV;

    return 16;
}

static int otir(Z80* cpu) {
    uint8_t val = cpu->mem_read_fn(cpu->HL);
    cpu->io_write_fn(cpu->BC, val);

    cpu->WZ = cpu->BC + 1;

    // Flags N, C, H selon Sean Young §2.2.6 pour OTI/OTIR
    uint16_t c_plus_1 = static_cast<uint16_t>(cpu->BC) + 1;
    uint32_t sum_ch = static_cast<uint32_t>(c_plus_1) + val;
    bool carry_ch = (sum_ch > 0xFFFF);

    // P/V flag : même table que INI mais pour OTI
    bool c_bit0 = cpu->BC & 1;
    bool c_bit1 = cpu->BC & 2;
    bool inp_bit0 = val & 1;
    bool inp_bit1 = val & 2;
    bool inp_bit2 = val & 4;
    bool c_bit2 = cpu->BC & 4;

    // Table Temp1 pour OTI (même que INI)
    bool temp1 = ((c_bit1 && !c_bit0 && inp_bit0 && inp_bit1) ||
                  (!c_bit1 && c_bit0 && inp_bit1) ||
                  (c_bit1 && c_bit0 && inp_bit0) ||
                  (c_bit1 && c_bit0 && inp_bit1));

    bool parity_b = !((cpu->B >> 0 & 1) ^ (cpu->B >> 1 & 1) ^ (cpu->B >> 2 & 1) ^
                      (cpu->B >> 3 & 1) ^ (cpu->B >> 4 & 1) ^ (cpu->B >> 5 & 1) ^
                      (cpu->B >> 6 & 1) ^ (cpu->B >> 7 & 1));
    bool low_b_nonzero = cpu->B & 0x0F;
    bool temp2;
    if (!low_b_nonzero) {
        temp2 = parity_b ^ ((cpu->B >> 4 & 1) || ((cpu->B >> 6 & 1) && !(cpu->B >> 5 & 1)));
    } else {
        temp2 = parity_b ^ ((cpu->B >> 0 & 1) || ((cpu->B >> 2 & 1) && !(cpu->B >> 1 & 1)));
    }

    bool pv_flag = temp1 ^ temp2 ^ c_bit2 ^ inp_bit2;

    cpu->B--;
    cpu->HL++;

    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C);
    if (cpu->B == 0)   cpu->F |= FLAG_Z;
    if (cpu->B & 0x80) cpu->F |= FLAG_S;
    if (carry_ch)      cpu->F |= FLAG_H | FLAG_C;
    cpu->F |= (val & 0x80) ? FLAG_N : 0;
    if (pv_flag)       cpu->F |= FLAG_PV;

    bool done = (cpu->B == 0);
    if (!done) {
        cpu->PC -= 2;
        return 16 + 21;
    }
    return 16 + 5;
}

static int indr(Z80* cpu) {
    uint8_t val = cpu->io_read_fn(cpu->BC);
    cpu->mem_write_fn(cpu->HL, val);

    cpu->WZ = cpu->BC + 1;

    // Flags N, C, H pour INDR/INDR
    uint16_t c_minus_1 = static_cast<uint16_t>(cpu->BC) - 1;
    uint32_t sum_ch = static_cast<uint32_t>(c_minus_1) + val;
    bool carry_ch = (sum_ch > 0xFFFF);

    // P/V flag : table Temp1 différente pour IND/INDR
    bool c_bit0 = cpu->BC & 1;
    bool c_bit1 = cpu->BC & 2;
    bool inp_bit0 = val & 1;
    bool inp_bit1 = val & 2;
    bool inp_bit2 = val & 4;
    bool c_bit2 = cpu->BC & 4;

    // Table Temp1 pour IND/INDR (différente de INI)
    bool temp1_indr = ((!c_bit1 && !c_bit0 && inp_bit0 && inp_bit1) ||
                       (!c_bit1 && !c_bit0 && inp_bit1 && !inp_bit0) ||
                       (!c_bit1 && c_bit0 && !inp_bit1 && inp_bit0) ||
                       (c_bit1 && !c_bit0 && inp_bit0));

    bool parity_b = !((cpu->B >> 0 & 1) ^ (cpu->B >> 1 & 1) ^ (cpu->B >> 2 & 1) ^
                      (cpu->B >> 3 & 1) ^ (cpu->B >> 4 & 1) ^ (cpu->B >> 5 & 1) ^
                      (cpu->B >> 6 & 1) ^ (cpu->B >> 7 & 1));
    bool low_b_nonzero = cpu->B & 0x0F;
    bool temp2;
    if (!low_b_nonzero) {
        temp2 = parity_b ^ ((cpu->B >> 4 & 1) || ((cpu->B >> 6 & 1) && !(cpu->B >> 5 & 1)));
    } else {
        temp2 = parity_b ^ ((cpu->B >> 0 & 1) || ((cpu->B >> 2 & 1) && !(cpu->B >> 1 & 1)));
    }

    bool pv_flag = temp1_indr ^ temp2 ^ c_bit2 ^ inp_bit2;

    cpu->B--;
    cpu->HL--;

    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C);
    if (cpu->B == 0)   cpu->F |= FLAG_Z;
    if (cpu->B & 0x80) cpu->F |= FLAG_S;
    if (carry_ch)      cpu->F |= FLAG_H | FLAG_C;
    cpu->F |= (val & 0x80) ? FLAG_N : 0;
    if (pv_flag)       cpu->F |= FLAG_PV;

    return 16;
}

static int inir(Z80* cpu) {
    uint8_t val = cpu->io_read_fn(cpu->BC);
    cpu->mem_write_fn(cpu->HL, val);

    cpu->WZ = cpu->BC + 1;

    // Flags N, C, H pour INIR
    uint16_t c_plus_1 = static_cast<uint16_t>(cpu->BC) + 1;
    uint32_t sum_ch = static_cast<uint32_t>(c_plus_1) + val;
    bool carry_ch = (sum_ch > 0xFFFF);

    // P/V flag : même table que INI
    bool c_bit0 = cpu->BC & 1;
    bool c_bit1 = cpu->BC & 2;
    bool inp_bit0 = val & 1;
    bool inp_bit1 = val & 2;
    bool inp_bit2 = val & 4;
    bool c_bit2 = cpu->BC & 4;

    bool temp1 = ((c_bit1 && !c_bit0 && inp_bit0 && inp_bit1) ||
                  (!c_bit1 && c_bit0 && inp_bit1) ||
                  (c_bit1 && c_bit0 && inp_bit0) ||
                  (c_bit1 && c_bit0 && inp_bit1));

    bool parity_b = !((cpu->B >> 0 & 1) ^ (cpu->B >> 1 & 1) ^ (cpu->B >> 2 & 1) ^
                      (cpu->B >> 3 & 1) ^ (cpu->B >> 4 & 1) ^ (cpu->B >> 5 & 1) ^
                      (cpu->B >> 6 & 1) ^ (cpu->B >> 7 & 1));
    bool low_b_nonzero = cpu->B & 0x0F;
    bool temp2;
    if (!low_b_nonzero) {
        temp2 = parity_b ^ ((cpu->B >> 4 & 1) || ((cpu->B >> 6 & 1) && !(cpu->B >> 5 & 1)));
    } else {
        temp2 = parity_b ^ ((cpu->B >> 0 & 1) || ((cpu->B >> 2 & 1) && !(cpu->B >> 1 & 1)));
    }

    bool pv_flag = temp1 ^ temp2 ^ c_bit2 ^ inp_bit2;

    cpu->B--;
    cpu->HL++;

    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C);
    if (cpu->B == 0)   cpu->F |= FLAG_Z;
    if (cpu->B & 0x80) cpu->F |= FLAG_S;
    if (carry_ch)      cpu->F |= FLAG_H | FLAG_C;
    cpu->F |= (val & 0x80) ? FLAG_N : 0;
    if (pv_flag)       cpu->F |= FLAG_PV;

    bool done = (cpu->B == 0);
    if (!done) {
        cpu->PC -= 2;
        return 16 + 21;
    }
    return 16 + 5;
}

// ============================================================================
// Dispatcher ED principal — décodage par opcode direct
// Bug #G FIX : adc/sbc_hl_rp utilisent uint32_t pour carry
// Bug #H FIX : LDIR/LDDR avec PC rewind quand BC≠0, return 21/16
// Bug #I FIX : CPI/CPDR PV = BC≠0, pas parité du résultat
// ============================================================================

int z80_exec_ed(Z80* cpu) {
    uint8_t opcode = z80_fetch_byte(cpu);

    switch (opcode) {
        // === IN r,(C) / OUT (C),r — Bug #13 FIX : flags complets ===
        case 0x40: { // IN B,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->B = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x41: { // OUT (C),B
            cpu->io_write_fn(cpu->BC, cpu->B);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        case 0x48: { // IN C,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->C = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x49: { // OUT (C),C
            cpu->io_write_fn(cpu->BC, cpu->C);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        case 0x50: { // IN D,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->D = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x51: { // OUT (C),D
            cpu->io_write_fn(cpu->BC, cpu->D);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        case 0x58: { // IN E,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->E = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x59: { // OUT (C),E
            cpu->io_write_fn(cpu->BC, cpu->E);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }

        // === LD I,A / LD R,A — timing correct = 9 T-states ===
        case 0x47: { // LD I,A
            cpu->I = cpu->A;
#ifdef GALAXIAN_DEBUG_IM2
            printf("[IM2-SETUP] PC=%04X ED47 LD I,A → I=%02X\n", cpu->PC - 1, cpu->I);
#endif
            return 9;
        }
        case 0x4F: { // LD R,A
            cpu->R = cpu->A;
            return 9;
        }
        case 0x57: { // LD A,I
            cpu->A = cpu->I;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
            if (cpu->A == 0)   cpu->F |= FLAG_Z;
            if (cpu->A & 0x80) cpu->F |= FLAG_S;
            if (cpu->IFF2)     cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, cpu->A);
            return 9;
        }
        case 0x5F: { // LD A,R
            cpu->A = cpu->R;
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
            if (cpu->A == 0)   cpu->F |= FLAG_Z;
            if (cpu->A & 0x80) cpu->F |= FLAG_S;
            if (cpu->IFF2)     cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, cpu->A);
            return 9;
        }

        // === IM modes d'interruption — timing correct = 8 T-states ===
        // ED 46=IM 0, ED 56=IM 1, ED 5E=IM 2 : laisser le jeu configurer librement.
        // Les alias non documentés (0x4E/0x66/0x6E/0x76/0x7E) forcent IM 1 comme sur Z80 réel.
        case 0x46: { cpu->IM = 0; return 8; }   // IM 0
        case 0x56: { cpu->IM = 1; return 8; }   // IM 1
        case 0x5E: { 
#ifdef GALAXIAN_DEBUG_IM2
            printf("[IM2-SETUP] PC=%04X ED5E IM 2\n", cpu->PC - 1);
#endif
            cpu->IM = 2; return 8; 
        }   // IM 2 — CORRECT
        // Alias non documentés → forcent IM 1 (comportement Z80 réel)
        case 0x4E: { cpu->IM = 1; return 8; }
        case 0x66: { cpu->IM = 1; return 8; }
        case 0x6E: { cpu->IM = 1; return 8; }
        case 0x76: { cpu->IM = 1; return 8; }
        case 0x7E: { cpu->IM = 1; return 8; }

        // === ADC HL,rp — Bug #G FIX : opcodes corrects + uint32_t ===
        case 0x4A: return adc_hl_rp(cpu, cpu->BC);   // ADC HL,BC
        case 0x5A: return adc_hl_rp(cpu, cpu->DE);   // ADC HL,DE
        case 0x6A: return adc_hl_rp(cpu, cpu->HL);   // ADC HL,HL

        // === SBC HL,rp — Bug #G FIX : opcodes corrects + uint32_t ===
        case 0x42: return sbc_hl_rp(cpu, cpu->BC);   // SBC HL,BC
        case 0x52: return sbc_hl_rp(cpu, cpu->DE);   // SBC HL,DE
        case 0x62: return sbc_hl_rp(cpu, cpu->HL);   // SBC HL,HL
        case 0x72: return sbc_hl_rp(cpu, cpu->SP);   // SBC HL,SP

        // === LD (nn),rp / LD rp,(nn) ===
        case 0x43: { // LD (nn),BC
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->BC & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->BC >> 8) & 0xFF));
            return 20;
        }
        case 0x4B: { // LD BC,(nn)
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->BC = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }
        case 0x53: { // LD (nn),DE
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->DE & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->DE >> 8) & 0xFF));
            return 20;
        }
        case 0x5B: { // LD DE,(nn)
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->DE = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }
        case 0x63: { // LD (nn),HL
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->HL & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->HL >> 8) & 0xFF));
            return 20;
        }
        case 0x6B: { // LD HL,(nn)
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->HL = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }
        case 0x73: { // LD (nn),SP
            uint16_t addr = z80_fetch_word(cpu);
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->SP & 0xFF));
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->SP >> 8) & 0xFF));
            return 20;
        }
        case 0x7B: { // LD SP,(nn)
            uint16_t addr = z80_fetch_word(cpu);
            uint8_t lo = cpu->mem_read_fn(addr);
            uint8_t hi = cpu->mem_read_fn(addr + 1);
            cpu->SP = static_cast<uint16_t>((hi << 8) | lo);
            return 20;
        }

        // ============================================================================
        // LDI/LDD — ED A0/A8 : une seule itération, mêmes flags que LDIR/LDDR
        // Section 2.2.5 Sean Young : X/Y depuis (byte_transféré + A)
        // ============================================================================

        case 0xA0: { // LDI (ED A0) — une itération sans boucle
            uint8_t val = cpu->mem_read_fn(cpu->HL);
            cpu->mem_write_fn(cpu->DE, val);
            cpu->HL++;
            cpu->DE++;
            cpu->BC--;

            cpu->F &= ~(FLAG_H | FLAG_N | FLAG_PV | FLAG_X | FLAG_Y);

            uint8_t sum = static_cast<uint8_t>(val + cpu->A);
            if (sum & 0x08) cpu->F |= FLAG_X;
            if (sum & 0x20) cpu->F |= FLAG_Y;
            // PV = BC != 0 après décrément (CORRIGÉ)
            if (cpu->BC != 0) cpu->F |= FLAG_PV;

            return 16;
        }
        case 0xA8: { // LDD (ED A8) — une itération sans boucle
            uint8_t val = cpu->mem_read_fn(cpu->HL);
            cpu->mem_write_fn(cpu->DE, val);
            cpu->HL--;
            cpu->DE--;
            cpu->BC--;

            cpu->F &= ~(FLAG_H | FLAG_N | FLAG_PV | FLAG_X | FLAG_Y);

            uint8_t sum = static_cast<uint8_t>(val + cpu->A);
            if (sum & 0x08) cpu->F |= FLAG_X;
            if (sum & 0x20) cpu->F |= FLAG_Y;
            // PV = BC != 0 après décrément (CORRIGÉ)
            if (cpu->BC != 0) cpu->F |= FLAG_PV;

            return 16;
        }

        // === Block transfer — LDIR, LDDR déplacés après conditionnels ===
        case 0xA1: { // CPI (ED A1) — déjà corrigé via fonction cpi() plus haut
            return cpi(cpu);
        }
        case 0xA9: { // CPD (ED A9) — une seule itération, PAS de boucle auto
            uint8_t val = cpu->mem_read_fn(cpu->HL);
            uint16_t result = static_cast<uint16_t>(cpu->A) - static_cast<uint16_t>(val);
            uint8_t res8 = static_cast<uint8_t>(result & 0xFF);

            bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;

            cpu->HL--;
            cpu->BC--;

            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
            if (cpu->A == val) cpu->F |= FLAG_Z;
            if (res8 & 0x80) cpu->F |= FLAG_S;
            if (half_borrow) cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;

            // PV = BC≠0 après décrément, pas parité du résultat
            if (cpu->BC != 0) cpu->F |= FLAG_PV;

            // X/Y depuis (A - val - H_flag)
            uint8_t temp = static_cast<uint8_t>(static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val) - (half_borrow ? 1 : 0));
            cpu->F &= ~(FLAG_X | FLAG_Y);
            if (temp & 0x08) cpu->F |= FLAG_X;
            if (temp & 0x20) cpu->F |= FLAG_Y;

            return 16;  // CPD = 16 cycles, pas de rewind PC
        }
        case 0xB1: { // CPIR (ED B1) — déjà corrigé via fonction cpir() plus haut
            return cpir(cpu);
        }

        // ============================================================================
        // CPDR — ED B9 : CPD avec boucle automatique
        // Section 2.2.5 Sean Young : mêmes flags que CPD + PC rewind quand BC≠0 et A!=(HL)
        // ============================================================================
        case 0xB9: { // CPDR (ED B9)
            uint8_t val = cpu->mem_read_fn(cpu->HL);
            uint16_t result = static_cast<uint16_t>(cpu->A) - static_cast<uint16_t>(val);
            uint8_t res8 = static_cast<uint8_t>(result & 0xFF);

            bool half_borrow = ((cpu->A & 0xF) - (val & 0xF)) < 0;

            cpu->HL--;
            cpu->BC--;

            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV);
            if (cpu->A == val) cpu->F |= FLAG_Z;
            if (res8 & 0x80) cpu->F |= FLAG_S;
            if (half_borrow) cpu->F |= FLAG_H;
            cpu->F |= FLAG_N;

            // PV = BC≠0, pas parité du résultat
            if (cpu->BC != 0) cpu->F |= FLAG_PV;

            // X/Y depuis (A - val - H_flag)
            uint8_t temp = static_cast<uint8_t>(static_cast<int16_t>(cpu->A) - static_cast<int16_t>(val) - (half_borrow ? 1 : 0));
            cpu->F &= ~(FLAG_X | FLAG_Y);
            if (temp & 0x08) cpu->F |= FLAG_X;
            if (temp & 0x20) cpu->F |= FLAG_Y;

            bool done = (cpu->BC == 0 || (cpu->A == val));
            if (!done) {
                cpu->PC -= 2;
                return 16 + 21;
            }
            return 16 + 5;
        }

        // === JP cc,nn — opcodes ED B4/BC/B5/B7 (supprimer doublons) ===
        case 0xB4: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t;
            if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = t; return 10; } return 10; } // JP NZ,nn (ED B4)
        case 0xBC: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t;
            if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = t; return 10; } return 10; } // JP Z,nn
        case 0xB5: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t;
            if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC = t; return 10; } return 10; } // JP NC,nn
        case 0xBD: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t;
            if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC = t; return 10; } return 10; } // JP C,nn

        // === Block transfer — LDIR, LDDR ===
        case 0xB0: return ldir(cpu);    // LDIR (ED B0)
        case 0xB8: return lddr(cpu);    // LDDR (ED B8)

        // === Block I/O — opcodes corrects ===
        case 0xB3: return otir(cpu);   // OTIR (ED B3)
        case 0xBB: { // OTDR (ED BB)
            cpu->io_write_fn(cpu->BC, cpu->mem_read_fn(cpu->HL));
            cpu->WZ = static_cast<uint16_t>(cpu->BC + 1);
            cpu->B--; cpu->HL--;
            cpu->F &= ~(FLAG_S | FLAG_Z);
            if (cpu->B == 0) cpu->F |= FLAG_Z;
            if (cpu->B & 0x80) cpu->F |= FLAG_S;
            return 16; }
        case 0xBA: return indr(cpu);   // INDR (ED BA)
        case 0xB2: return inir(cpu);   // INIR (ED B2)

        // === IN A,(C) / OUT (C),A ===
        case 0x60: { // IN H,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->H = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x61: { // OUT (C),H
            cpu->io_write_fn(cpu->BC, cpu->H);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        case 0x68: { // IN L,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->L = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x69: { // OUT (C),L
            cpu->io_write_fn(cpu->BC, cpu->L);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        // ============================================================================
        // IN F,(C) — ED 70 : lit le port, met à jour les flags comme IN r,(C)
        // OUT (C),0 — ED 71 : envoie la valeur 0 sur le port I/O
        // ============================================================================

        case 0x70: { // IN F,(C) / IN (C) — ED 70
            uint8_t val = cpu->io_read_fn(cpu->BC);
            // Ne stocke rien, met juste à jour les flags
            cpu->F &= FLAG_C;  // Préserver C uniquement
            if (val == 0)      cpu->F |= FLAG_Z;
            if (val & 0x80)    cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            // H=0, N=0 pour IN
            cpu->WZ = cpu->BC + 1;
            return 12;
        }
        case 0x71: { // OUT (C),0 — ED 71 : envoie 0 sur le port
            cpu->io_write_fn(cpu->BC, 0);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }

        case 0x78: { // IN A,(C)
            uint8_t val = cpu->io_read_fn(cpu->BC);
            cpu->A = val;
            cpu->F &= FLAG_C;
            if (val == 0)    cpu->F |= FLAG_Z;
            if (val & 0x80)  cpu->F |= FLAG_S;
            if (z80_parity(val)) cpu->F |= FLAG_PV;
            z80_set_flags_xy(cpu, val);
            return 12;
        }
        case 0x79: { // OUT (C),A
            cpu->io_write_fn(cpu->BC, cpu->A);
            cpu->WZ = cpu->BC + 1;
            return 12;
        }

        // === NEG — ED 44 : A = 0 - A (deuxième complément) ===
        case 0x44: {
            uint8_t val = cpu->A;
            int16_t result = 0 - static_cast<int16_t>(val);
            uint8_t res8 = static_cast<uint8_t>(result & 0xFF);

            // Flags : S, Z, PV (overflow si val == 0x80), C (si val != 0)
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_X | FLAG_Y);
            if (res8 == 0)     cpu->F |= FLAG_Z;
            if (res8 & 0x80)   cpu->F |= FLAG_S;
            if (val != 0)      cpu->F |= FLAG_C;  // Carry si val != 0
            // CORRIGÉ : half-borrow si le nibble bas de A est non nul (0 - A)
            bool half_borrow = (val & 0x0F) != 0;
            if (half_borrow)   cpu->F |= FLAG_H;
            if (val == 0x80)   cpu->F |= FLAG_PV;  // Overflow signé : 0x80 → 0x80
            cpu->F |= FLAG_N;
            z80_set_flags_xy(cpu, res8);
            cpu->A = res8;
            return 8;
        }

        // === ADC HL,SP — ED 7A (CORRIGÉ : était CALL PE,nn, faux) ===
        case 0x7A: return adc_hl_rp(cpu, cpu->SP);   // ADC HL,SP

        // === Opcodes non documentés ED 80-83 → NOP (CORRIGÉ : n'étaient pas RET cc) ===
        case 0x80: return 8;  // NOP (non doc)
        case 0x81: return 8;  // NOP (non doc)
        case 0x82: return 8;  // NOP (non doc)
        case 0x83: return 8;  // NOP (non doc)

        // === RETI / RETN — ED 4D / 45 ===
        case 0x45: { // RETN — Retour de NMI
            uint16_t new_pc = z80_pop_word(cpu);
            cpu->PC = new_pc;
            cpu->WZ = new_pc;
            cpu->IFF1 = cpu->IFF2;
            if (cpu->nmi_return_fn)
                cpu->nmi_return_fn();
            cpu->NMI_in_service = false;
            printf("[RETN] PC=%04X -> %04X SP=%04X IFF1=%d\n",
                   cpu->PC, new_pc, cpu->SP, cpu->IFF1 ? 1 : 0);
            return 14;
        }
        case 0x4D: { // RETI — Retour d'interruption maskable
            uint16_t new_pc = z80_pop_word(cpu);
            cpu->PC = new_pc;
            cpu->WZ = new_pc;
            cpu->IFF1 = cpu->IFF2;
            return 14;
        }

        // === RLD / RRD — ED 6F / 67 ===
        case 0x67: { // RRD — Rotate Right Decimal
            // Rotation des nibbles :
            // A[7:4] → (HL)[7:4]  (nibble HAUT de A va dans nibble HAUT de (HL))
            // (HL)[3:0] → A[7:4]  (nibble BAS de (HL) va dans nibble HAUT de A)
            // (HL)[7:4] perd ses bits hauts
            
            uint8_t mem_val = cpu->mem_read_fn(cpu->HL);
            
            // new_A : nibble HAUT vient de (HL)[3:0], nibble BAS préservé de A[3:0]
            uint8_t new_a = (cpu->A & 0x0F) | ((mem_val & 0x0F) << 4);
            
            // new_mem : nibble HAUT vient de A[7:4], nibble BAS préservé de (HL)[3:0]
            uint8_t new_mem = ((cpu->A >> 4) << 4) | (mem_val & 0x0F);

            cpu->mem_write_fn(cpu->HL, new_mem);
            cpu->A = new_a;

            // Flags : S, Z, P selon nouveau A ; H=0 N=0
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (new_a == 0)      cpu->F |= FLAG_Z;
            if (new_a & 0x80)    cpu->F |= FLAG_S;
            if (z80_parity(new_a)) cpu->F |= FLAG_PV;
            cpu->F |= (new_a & (FLAG_X | FLAG_Y));
            return 18;
        }
        case 0x6F: { // RLD — Rotate Left Decimal
            // Rotation des nibbles :
            // (HL)[7:4] → A[3:0]  (nibble HAUT de (HL) va dans nibble BAS de A)
            // A[3:0] → (HL)[3:0]  (nibble BAS de A va dans nibble BAS de (HL))
            // (HL)[7:4] perd ses bits hauts
            
            uint8_t mem_val = cpu->mem_read_fn(cpu->HL);
            
            // new_A : nibble HAUT préservé de A[7:4], nibble BAS vient de (HL)[7:4]
            uint8_t new_a = (cpu->A & 0xF0) | (mem_val >> 4);
            
            // new_mem : nibble HAUT vient de (HL)[3:0], nible BAS vient de A[3:0]
            uint8_t new_mem = ((mem_val & 0x0F) << 4) | (cpu->A & 0x0F);

            cpu->mem_write_fn(cpu->HL, new_mem);
            cpu->A = new_a;

            // Flags : S, Z, P selon nouveau A ; H=0 N=0
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_N | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (new_a == 0)      cpu->F |= FLAG_Z;
            if (new_a & 0x80)    cpu->F |= FLAG_S;
            if (z80_parity(new_a)) cpu->F |= FLAG_PV;
            cpu->F |= (new_a & (FLAG_X | FLAG_Y));
            return 18;
        }

        // NOP pour ED non implémentés
        default:
            return 8;
    }
}