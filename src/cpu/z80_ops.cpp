#include <cstdio>
#include "../include/cpu/z80.h"
#include "../include/cpu/z80_ops.h"
#include "../include/cpu/z80_cb.h"
#include "../include/cpu/z80_dd.h"   // z80_exec_dd — Préfixe DD (IX index register)
#include "../include/cpu/z80_ed.h"   // z80_exec_ed — Préfixe ED (special instructions)
#include "../include/cpu/z80_fd.h"   // z80_exec_fd — Préfixe FD (IY index register)

// ============================================================================
// DEBUG TRACING — traceur d'opcodes pour débogage du boot Galaxian
// Variables globales définies ici, déclarées dans include/cpu/z80.h
// ============================================================================
bool g_opcode_trace_enabled = true;
OpcodeTraceEntry g_opcode_trace[MAX_OPCODE_TRACE];
int g_opcode_trace_count = 0;

// ============================================================================
// Zilog Z80 CPU Emulator — Instructions Principales (Sans Préfixe)
// ============================================================================
//
// Ce fichier implémente le décodeur principal du Z80 : toutes les instructions
// qui ne portent pas de préfixe spécial (CB, DD, ED, FD). Il couvre environ
// 128 opcodes uniques organisés en catégories :
//
//   Catégorie       | Opcodes | Description
//   ----------------|---------|------------------------------------------
//   NOP/Rotations   | 0x00-0F | NOP, RRCA, RLCA
//   Sauts           | 0x10-38 | DJNZ, JR, JP conditionnels/inconditionnels
//   INC/DEC r       | 0x04-3D | Incrémenter/Décrémenter registres 8-bit
//   LD r,n          | 0x06-3E | Charger octet immédiat dans registre
//   ALU             | 0x80-FE | ADD/ADC/SUB/SBC/AND/OR/XOR/CP
//   LD r,r'         | 0x40-7F | Transfert registre à registre (64 combos)
//   LD nn,rp        | 0x01-31 | Charger mot dans registre pair 16-bit
//   ADD HL,rp       | 0x09-39 | Addition 16-bit HL + registre pair
//   LD (nn),rp      | 0x22-2B | Transferts mémoire 16-bit
//   LD (xy),A       | 0x02-1A | Transferts A vers/depuis [BC]/[DE]
//   PUSH/POP        | 0xC1-F5 | Opérations de pile
//   RST             | 0xC7-FF | Sauts vers vecteurs de restart (8 adresses)
//   Flags           | 0x27-3F | DAA, CPL, SCF, CCF
//   Interrupts      | 0xFB-F3 | EI, DI
//   EX              | 0xE3-08 | EX (SP),HL / EX AF,AF' / EXX
//   HALT            | 0x76    | Mettre le CPU en pause
//   IN/OUT          | 0xDB-D3 | Entrées/Sorties I/O avec octet immédiat
//
// Référence : Zilog Z80 CPU User's Manual — Section "Instruction Set"
// ============================================================================

// ============================================================================
// Helpers d'accès registre (locaux au décodeur principal)
// ============================================================================
// Identiques à z80_read_r/z80_write_r mais inline pour performance.

/**
 * @brief Lit la valeur d'un registre 8-bit par index.
 * @param cpu  Pointeur vers la structure Z80
 * @param idx  Index registre (0=B, ..., 6=(HL), 7=A)
 * @return     Valeur du registre ou de la mémoire [HL]
 */
static inline uint8_t read_r(Z80* cpu, int idx) {
    switch (idx) {
        case 0: return cpu->B;                    // Index 0 → Registre B
        case 1: return cpu->C;                    // Index 1 → Registre C
        case 2: return cpu->D;                    // Index 2 → Registre D
        case 3: return cpu->E;                    // Index 3 → Registre E
        case 4: return cpu->H;                    // Index 4 → Registre H
        case 5: return cpu->L;                    // Index 5 → Registre L
        case 6: return cpu->mem_read_fn(cpu->HL); // Index 6 → Mémoire [HL] (accès indirect)
        case 7: return cpu->A;                    // Index 7 → Registre A
        default: return 0;                        // Valeur par défaut pour index invalide
    }
}

/**
 * @brief Écrit une valeur dans un registre 8-bit par index.
 * @param cpu     Pointeur vers la structure Z80
 * @param idx     Index registre (0=B, ..., 6=(HL), 7=A)
 * @param val     Valeur à écrire
 */
static inline void write_r(Z80* cpu, int idx, uint8_t val) {
    switch (idx) {
        case 0: cpu->B = val; break;            // Index 0 → Registre B
        case 1: cpu->C = val; break;            // Index 1 → Registre C
        case 2: cpu->D = val; break;            // Index 2 → Registre D
        case 3: cpu->E = val; break;            // Index 3 → Registre E
        case 4: cpu->H = val; break;            // Index 4 → Registre H
        case 5: cpu->L = val; break;            // Index 5 → Registre L
        case 6: cpu->mem_write_fn(cpu->HL, val); break; // Index 6 → Mémoire [HL] (accès indirect)
        case 7: cpu->A = val; break;            // Index 7 → Registre A
        default: break;                         // Index invalide : aucune action
    }
}

// ============================================================================
// Conditions de branchement — test des flags Z80
// ============================================================================
// Les instructions conditionnelles (JP cc,nn / JR cc,d / CALL cc,nn / RET cc)
// utilisent un code de condition de 3 bits (cc) :
//   0=NZ (Z=0), 1=Z (Z=1), 2=NC (C=0), 3=C (C=1)
//   4=PO (PV=0), 5=PE (PV=1), 6=P (S=0), 7=M (S=1)

/**
 * @brief Teste une condition de branchement basée sur les flags Z80.
 * @param cpu     Pointeur vers la structure Z80
 * @param cond    Code de condition (0-7) : NZ/Z/NC/C/PO/PE/P/M
 * @return         true si la condition est vraie, false sinon
 */
static inline bool test_cond(Z80* cpu, int cond) {
    switch (cond) {
        case 0: return !TEST_FLAG(cpu, FLAG_Z);   // NZ — Not Zero (Z=0)
        case 1: return  TEST_FLAG(cpu, FLAG_Z);   // Z — Zero (Z=1)
        case 2: return !TEST_FLAG(cpu, FLAG_C);   // NC — No Carry (C=0)
        case 3: return  TEST_FLAG(cpu, FLAG_C);   // C — Carry (C=1)
        case 4: return !TEST_FLAG(cpu, FLAG_PV);  // PO — Parity Odd (PV=0)
        case 5: return  TEST_FLAG(cpu, FLAG_PV);  // PE — Parity Even (PV=1)
        case 6: return !TEST_FLAG(cpu, FLAG_S);   // P — Positive/Plus (S=0)
        case 7: return  TEST_FLAG(cpu, FLAG_S);   // M — Negative/Moins (S=1)
        default: return false;                    // Condition invalide → toujours faux
    }
}

// ============================================================================
// Décodeur principal — z80_exec_main
// ============================================================================
// Cette fonction est le cœur du décodage des instructions Z80 sans préfixe.
// Chaque opcode est traité dans un switch/case avec son implémentation exacte.
// Retourne les T-states (cycles machine) consommés par l'instruction.

/**
 * @brief Décode et exécute une instruction principale (sans préfixe).
 *
 * Cette fonction contient le décodage de ~128 opcodes Z80 organisés en :
 *   - NOP / Rotations accumulateur (RLCA, RRCA)
 *   - Sauts conditionnels/inconditionnels (JP, JR, CALL, RET, RST)
 *   - INC / DEC 8-bit et 16-bit
 *   - LD registre à registre, registre immédiat, mémoire
 *   - ALU : ADD/ADC/SUB/SBC/AND/OR/XOR/CP
 *   - PUSH / POP — Opérations de pile
 *   - Flags : DAA, CPL, SCF, CCF
 *   - Interrupts : EI, DI
 *   - EX : AF,AF' / EXX / (SP),HL
 *   - HALT — Mise en pause du CPU
 *   - IN/OUT — Entrées/Sorties I/O avec octet immédiat
 *
 * @param cpu     Pointeur vers la structure Z80
 * @param opcode  Opcode principal lu depuis la mémoire (sans préfixe)
 * @return        Nombre de T-states (cycles machine) consommés
 */

int z80_exec_main(Z80* cpu, uint8_t opcode) {
    // DEBUG : tracer chaque opcode exécuté
    if (g_opcode_trace_enabled && g_opcode_trace_count < MAX_OPCODE_TRACE) {
        g_opcode_trace[g_opcode_trace_count].pc_before = cpu->PC;
        g_opcode_trace[g_opcode_trace_count].opcode = opcode;
        g_opcode_trace_count++;
    }

    switch (opcode) {
        // ====================================================================
        // NOP / RRCA / RLCA — Instructions sans effet ou rotations accumulateur
        // ====================================================================

        case 0x00: return 4;  // NOP — No Operation : ne fait rien, consomme 4 T-states
        case 0x07: { z80_rot_rlca(cpu); return 4; }   // RLCA — Rotate Accumulator Left : rotation circulaire gauche de A
        case 0x0F: { z80_rot_rrca(cpu); return 4; }   // RRCA — Rotate Accumulator Right : rotation circulaire droite de A

        // ====================================================================
        // DJNZ d — Decrement B, Jump if Not Zero
        // ====================================================================
        // Décrémente B. Si B≠0, saut relatif signé 8-bit.
        // Timing : 8 T-states si non pris (B==0), 13 T-states si pris (B≠0).

        case 0x10: { // DJNZ d
            int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); // Lire le déplacement signé
            cpu->B--;                                        // Décrémenter B
            if (cpu->B != 0) {                               // Si B≠0 après décrément
                cpu->PC += d;                                // Saut relatif : PC += d
                cpu->WZ = cpu->PC;                           // Mettre à jour WZ (MEMPTR)
                return 13;   // DJNZ pris (B≠0) = 13 T-states
            }
            return 8;        // DJNZ non pris (B==0) = 8 T-states
        }

        // ====================================================================
        // INC/DEC r — Incrémenter/Décrémenter registres 8-bit
        // ====================================================================
        // Chaque registre a son opcode dédié : B(04/05), C(0C/0D), D(14/15),
        // E(1C/1D), H(24/25), L(2C/2D), (HL)(34/35), A(3C/3D).

        case 0x04: { z80_inc_r(cpu, 0); return 4; }   // INC B — Incrémenter B
        case 0x05: { z80_dec_r(cpu, 0); return 4; }   // DEC B — Décrémenter B
        case 0x0C: { z80_inc_r(cpu, 1); return 4; }   // INC C — Incrémenter C
        case 0x0D: { z80_dec_r(cpu, 1); return 4; }   // DEC C — Décrémenter C
        case 0x14: { z80_inc_r(cpu, 2); return 4; }   // INC D — Incrémenter D
        case 0x15: { z80_dec_r(cpu, 2); return 4; }   // DEC D — Décrémenter D
        case 0x1C: { z80_inc_r(cpu, 3); return 4; }   // INC E — Incrémenter E
        case 0x1D: { z80_dec_r(cpu, 3); return 4; }   // DEC E — Décrémenter E
        case 0x24: { z80_inc_r(cpu, 4); return 4; }   // INC H — Incrémenter H
        case 0x25: { z80_dec_r(cpu, 4); return 4; }   // DEC H — Décrémenter H
        case 0x2C: { z80_inc_r(cpu, 5); return 4; }   // INC L — Incrémenter L
        case 0x2D: { z80_dec_r(cpu, 5); return 4; }   // DEC L — Décrémenter L

        // INC/DEC (HL) — Accès mémoire via HL : plus lent (12 T-states au lieu de 4)
        case 0x34: { // INC (HL) — Incrémenter la valeur en mémoire [HL]
            uint8_t val    = cpu->mem_read_fn(cpu->HL);   // Lire la valeur actuelle
            uint8_t result = static_cast<uint8_t>(val + 1); // Incrémenter
            bool half_carry = (val & 0xF) == 0xF;         // Demi-carry : nibble bas débordé

            cpu->F &= FLAG_C;                             // Préserver uniquement C
            if (result == 0)    cpu->F |= FLAG_Z;        // Zero flag : wrap-around 0xFF→0x00
            if (result & 0x80)  cpu->F |= FLAG_S;        // Sign flag : résultat négatif
            if (half_carry)     cpu->F |= FLAG_H;        // Half carry : nibble bas débordé
            if (val == 0x7F)    cpu->F |= FLAG_PV;       // Overflow : 127→-128 (signé)
            z80_set_flags_xy(cpu, result);               // Flags X/Y undocumented
            cpu->mem_write_fn(cpu->HL, result);          // Écrire le résultat en mémoire [HL]
            return 12;                                   // 12 T-states pour accès mémoire
        }

        case 0x35: { // DEC (HL) — Décrémenter la valeur en mémoire [HL]
            uint8_t val    = cpu->mem_read_fn(cpu->HL);   // Lire la valeur actuelle
            uint8_t result = static_cast<uint8_t>(val - 1); // Décrémenter
            bool half_borrow = (val & 0xF) == 0x0;        // Demi-borrow : nibble bas emprunté

            cpu->F &= FLAG_C;                             // Préserver uniquement C
            if (result == 0)    cpu->F |= FLAG_Z;        // Zero flag : wrap-around 0x00→0xFF
            if (result & 0x80)  cpu->F |= FLAG_S;        // Sign flag : résultat négatif
            if (half_borrow)    cpu->F |= FLAG_H;        // Half borrow : nibble bas emprunté
            if (val == 0x80)    cpu->F |= FLAG_PV;       // Overflow : -128→127 (signé)
            cpu->F |= FLAG_N;                             // N=1 pour soustraction
            z80_set_flags_xy(cpu, result);               // Flags X/Y undocumented
            cpu->mem_write_fn(cpu->HL, result);          // Écrire le résultat en mémoire [HL]
            return 12;                                   // 12 T-states pour accès mémoire
        }

        case 0x3C: { z80_inc_r(cpu, 7); return 4; }   // INC A — Incrémenter A
        case 0x3D: { z80_dec_r(cpu, 7); return 4; }   // DEC A — Décrémenter A

        // ====================================================================
        // LD r,n — Charger octet immédiat dans registre
        // ====================================================================
        // Chaque registre a son opcode : B(06), C(0E), D(16), E(1E), H(26), L(2E), (HL)(36), A(3E).

        case 0x06: { cpu->B = z80_fetch_byte(cpu); return 7; }   // LD B,n — Charger octet immédiat dans B
        case 0x0E: { cpu->C = z80_fetch_byte(cpu); return 7; }   // LD C,n — Charger octet immédiat dans C
        case 0x16: { cpu->D = z80_fetch_byte(cpu); return 7; }   // LD D,n — Charger octet immédiat dans D
        case 0x1E: { cpu->E = z80_fetch_byte(cpu); return 7; }   // LD E,n — Charger octet immédiat dans E
        case 0x26: { cpu->H = z80_fetch_byte(cpu); return 7; }   // LD H,n — Charger octet immédiat dans H
        case 0x2E: { cpu->L = z80_fetch_byte(cpu); return 7; }   // LD L,n — Charger octet immédiat dans L

        case 0x36: { // LD (HL),n — Charger octet immédiat en mémoire [HL]
            uint8_t val = z80_fetch_byte(cpu);               // Lire l'octet immédiat
            cpu->mem_write_fn(cpu->HL, val);                 // Écrire en mémoire [HL]
            return 10;                                       // 10 T-states pour accès mémoire
        }

        case 0x3E: { cpu->A = z80_fetch_byte(cpu); return 7; }   // LD A,n — Charger octet immédiat dans A

        // ====================================================================
        // ADD/ADC/SUB/SBC/AND/OR/XOR/CP n/r — Instructions ALU avec opérande immédiate ou registre
        // ====================================================================
        // Les instructions sont groupées par type d'opération :
        //   ADD A,n (C6) / ADC A,n (CE) / SUB n (D6) / SBC A,n (DE)
        //   AND n (E6) / OR n (F6) / XOR n (EE) / CP n (FE)
        //   ADD/ADC/SUB/SBC/AND/OR/XOR/CP r (80-BF)

        case 0xC6: { uint8_t n = z80_fetch_byte(cpu); z80_alu_add(cpu, n); return 7; }   // ADD A,n — Addition octet immédiat à A
        case 0xCE: { uint8_t n = z80_fetch_byte(cpu); z80_alu_adc(cpu, n); return 7; }   // ADC A,n — Addition avec carry à A
        case 0xD6: { uint8_t n = z80_fetch_byte(cpu); z80_alu_sub(cpu, n); return 7; }   // SUB n — Soustraire octet immédiat de A
        case 0xDE: { uint8_t n = z80_fetch_byte(cpu); z80_alu_sbc(cpu, n); return 7; }   // SBC A,n — Soustraction avec borrow de A
        case 0xE6: { uint8_t n = z80_fetch_byte(cpu); z80_alu_and(cpu, n); return 7; }   // AND n — ET logique avec octet immédiat
        case 0xEE: { uint8_t n = z80_fetch_byte(cpu); z80_alu_xor(cpu, n); return 7; }   // XOR n — OU exclusif avec octet immédiat
        case 0xF6: { uint8_t n = z80_fetch_byte(cpu); z80_alu_or(cpu, n); return 7; }   // OR n — OU logique avec octet immédiat
        case 0xFE: { uint8_t n = z80_fetch_byte(cpu); z80_alu_cp(cpu, n); return 7; }   // CP n — Comparer A avec octet immédiat

        // ADD A,r / ADC A,r / SUB r / SBC A,r / AND r / XOR r / OR r (0x80-0xBF)
        // Chaque registre a son opcode : B(80), C(81), D(82), E(83), H(84), L(85), (HL)(86), A(87)

        case 0x80: { z80_alu_add(cpu, cpu->B); return 4; }   // ADD A,B — Addition B à A
        case 0x81: { z80_alu_add(cpu, cpu->C); return 4; }   // ADD A,C — Addition C à A
        case 0x82: { z80_alu_add(cpu, cpu->D); return 4; }   // ADD A,D — Addition D à A
        case 0x83: { z80_alu_add(cpu, cpu->E); return 4; }   // ADD A,E — Addition E à A
        case 0x84: { z80_alu_add(cpu, cpu->H); return 4; }   // ADD A,H — Addition H à A
        case 0x85: { z80_alu_add(cpu, cpu->L); return 4; }   // ADD A,L — Addition L à A
        case 0x86: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_add(cpu, v); return 7; }   // ADD A,(HL) — Addition [HL] à A
        case 0x87: { z80_alu_add(cpu, cpu->A); return 4; }   // ADD A,A — Addition A à A

        case 0x88: { z80_alu_adc(cpu, cpu->B); return 4; }   // ADC A,B — Addition avec carry B à A
        case 0x89: { z80_alu_adc(cpu, cpu->C); return 4; }   // ADC A,C — Addition avec carry C à A
        case 0x8A: { z80_alu_adc(cpu, cpu->D); return 4; }   // ADC A,D — Addition avec carry D à A
        case 0x8B: { z80_alu_adc(cpu, cpu->E); return 4; }   // ADC A,E — Addition avec carry E à A
        case 0x8C: { z80_alu_adc(cpu, cpu->H); return 4; }   // ADC A,H — Addition avec carry H à A
        case 0x8D: { z80_alu_adc(cpu, cpu->L); return 4; }   // ADC A,L — Addition avec carry L à A
        case 0x8E: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_adc(cpu, v); return 7; }   // ADC A,(HL) — Addition avec carry [HL] à A
        case 0x8F: { z80_alu_adc(cpu, cpu->A); return 4; }   // ADC A,A — Addition avec carry A à A

        case 0x90: { z80_alu_sub(cpu, cpu->B); return 4; }   // SUB B — Soustraire B de A
        case 0x91: { z80_alu_sub(cpu, cpu->C); return 4; }   // SUB C — Soustraire C de A
        case 0x92: { z80_alu_sub(cpu, cpu->D); return 4; }   // SUB D — Soustraire D de A
        case 0x93: { z80_alu_sub(cpu, cpu->E); return 4; }   // SUB E — Soustraire E de A
        case 0x94: { z80_alu_sub(cpu, cpu->H); return 4; }   // SUB H — Soustraire H de A
        case 0x95: { z80_alu_sub(cpu, cpu->L); return 4; }   // SUB L — Soustraire L de A
        case 0x96: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_sub(cpu, v); return 7; }   // SUB (HL) — Soustraire [HL] de A
        case 0x97: { z80_alu_sub(cpu, cpu->A); return 4; }   // SUB A — A - A = 0

        case 0x98: { z80_alu_sbc(cpu, cpu->B); return 4; }   // SBC A,B — Soustraction avec borrow B de A
        case 0x99: { z80_alu_sbc(cpu, cpu->C); return 4; }   // SBC A,C — Soustraction avec borrow C de A
        case 0x9A: { z80_alu_sbc(cpu, cpu->D); return 4; }   // SBC A,D — Soustraction avec borrow D de A
        case 0x9B: { z80_alu_sbc(cpu, cpu->E); return 4; }   // SBC A,E — Soustraction avec borrow E de A
        case 0x9C: { z80_alu_sbc(cpu, cpu->H); return 4; }   // SBC A,H — Soustraction avec borrow H de A
        case 0x9D: { z80_alu_sbc(cpu, cpu->L); return 4; }   // SBC A,L — Soustraction avec borrow L de A
        case 0x9E: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_sbc(cpu, v); return 7; }   // SBC A,(HL) — Soustraction avec borrow [HL] de A
        case 0x9F: { z80_alu_sbc(cpu, cpu->A); return 4; }   // SBC A,A — A - borrow

        case 0xA0: { z80_alu_and(cpu, cpu->B); return 4; }   // AND B — ET logique avec B
        case 0xA1: { z80_alu_and(cpu, cpu->C); return 4; }   // AND C — ET logique avec C
        case 0xA2: { z80_alu_and(cpu, cpu->D); return 4; }   // AND D — ET logique avec D
        case 0xA3: { z80_alu_and(cpu, cpu->E); return 4; }   // AND E — ET logique avec E
        case 0xA4: { z80_alu_and(cpu, cpu->H); return 4; }   // AND H — ET logique avec H
        case 0xA5: { z80_alu_and(cpu, cpu->L); return 4; }   // AND L — ET logique avec L
        case 0xA6: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_and(cpu, v); return 7; }   // AND (HL) — ET logique avec [HL]
        case 0xA7: { z80_alu_and(cpu, cpu->A); return 4; }   // AND A — A AND A

        case 0xA8: { z80_alu_xor(cpu, cpu->B); return 4; }   // XOR B — OU exclusif avec B
        case 0xA9: { z80_alu_xor(cpu, cpu->C); return 4; }   // XOR C — OU exclusif avec C
        case 0xAA: { z80_alu_xor(cpu, cpu->D); return 4; }   // XOR D — OU exclusif avec D
        case 0xAB: { z80_alu_xor(cpu, cpu->E); return 4; }   // XOR E — OU exclusif avec E
        case 0xAC: { z80_alu_xor(cpu, cpu->H); return 4; }   // XOR H — OU exclusif avec H
        case 0xAD: { z80_alu_xor(cpu, cpu->L); return 4; }   // XOR L — OU exclusif avec L
        case 0xAE: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_xor(cpu, v); return 7; }   // XOR (HL) — OU exclusif avec [HL]
        case 0xAF: { z80_alu_xor(cpu, cpu->A); return 4; }   // XOR A — A XOR A = 0

        case 0xB0: { z80_alu_or(cpu, cpu->B); return 4; }   // OR B — OU logique avec B
        case 0xB1: { z80_alu_or(cpu, cpu->C); return 4; }   // OR C — OU logique avec C
        case 0xB2: { z80_alu_or(cpu, cpu->D); return 4; }   // OR D — OU logique avec D
        case 0xB3: { z80_alu_or(cpu, cpu->E); return 4; }   // OR E — OU logique avec E
        case 0xB4: { z80_alu_or(cpu, cpu->H); return 4; }   // OR H — OU logique avec H
        case 0xB5: { z80_alu_or(cpu, cpu->L); return 4; }   // OR L — OU logique avec L
        case 0xB6: { uint8_t v = cpu->mem_read_fn(cpu->HL); z80_alu_or(cpu, v); return 7; }   // OR (HL) — OU logique avec [HL]
        case 0xB7: { z80_alu_or(cpu, cpu->A); return 4; }   // OR A — A OR A

        // ====================================================================
        // CP r — Comparer registre avec Accumulateur (0xB8-0xBF)
        // ====================================================================
        // Effectue A - r sans stocker le résultat. Flags positionnés comme SUB.

        case 0xB8: { z80_alu_cp(cpu, cpu->B); return 4; }   // CP B — Comparer A avec B
        case 0xB9: { z80_alu_cp(cpu, cpu->C); return 4; }   // CP C — Comparer A avec C
        case 0xBA: { z80_alu_cp(cpu, cpu->D); return 4; }   // CP D — Comparer A avec D
        case 0xBB: { z80_alu_cp(cpu, cpu->E); return 4; }   // CP E — Comparer A avec E
        case 0xBC: { z80_alu_cp(cpu, cpu->H); return 4; }   // CP H — Comparer A avec H
        case 0xBD: { z80_alu_cp(cpu, cpu->L); return 4; }   // CP L — Comparer A avec L
        case 0xBE: {
            uint8_t v = cpu->mem_read_fn(cpu->HL);
            z80_alu_cp(cpu, v);
            return 7;
        }   // CP (HL) — Comparer A avec [HL]
        case 0xBF: { z80_alu_cp(cpu, cpu->A); return 4; }   // CP A — Comparer A avec A = 0

        // ====================================================================
        // LD r,r' — Transfert registre à registre (64 combinaisons)
        // ====================================================================
        // Format : LD target,source où target et source sont dans {B,C,D,E,H,L,(HL),A}.
        // Groupées par registre cible : B(0x40-0x47), C(0x48-0x4F), D(0x50-0x57), etc.

        // LD B,r — Charger un registre dans B (0x40-0x47)
        case 0x40: return 4;                                                          // LD B,B — B ← B (NOP)
        case 0x41: { cpu->B = cpu->C; return 4; }                                    // LD B,C — B ← C
        case 0x42: { cpu->B = cpu->D; return 4; }                                    // LD B,D — B ← D
        case 0x43: { cpu->B = cpu->E; return 4; }                                    // LD B,E — B ← E
        case 0x44: { cpu->B = cpu->H; return 4; }                                    // LD B,H — B ← H
        case 0x45: { cpu->B = cpu->L; return 4; }                                    // LD B,L — B ← L

        case 0x46: { cpu->B = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD B,(HL) — B ← [HL] (timing : 7 T-states pour accès mémoire)
        case 0x47: { cpu->B = cpu->A; return 4; }                                    // LD B,A — B ← A

        // LD C,r — Charger un registre dans C (0x48-0x4F)
        case 0x48: { cpu->C = cpu->B; return 4; }                                    // LD C,B — C ← B
        case 0x49: return 4;                                                          // LD C,C — C ← C (NOP)
        case 0x4A: { cpu->C = cpu->D; return 4; }                                    // LD C,D — C ← D
        case 0x4B: { cpu->C = cpu->E; return 4; }                                    // LD C,E — C ← E
        case 0x4C: { cpu->C = cpu->H; return 4; }                                    // LD C,H — C ← H
        case 0x4D: { cpu->C = cpu->L; return 4; }                                    // LD C,L — C ← L

        case 0x4E: { cpu->C = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD C,(HL) — C ← [HL]
        case 0x4F: { cpu->C = cpu->A; return 4; }                                    // LD C,A — C ← A

        // LD D,r — Charger un registre dans D (0x50-0x57)
        case 0x50: { cpu->D = cpu->B; return 4; }                                    // LD D,B — D ← B
        case 0x51: { cpu->D = cpu->C; return 4; }                                    // LD D,C — D ← C
        case 0x52: return 4;                                                          // LD D,D — D ← D (NOP)
        case 0x53: { cpu->D = cpu->E; return 4; }                                    // LD D,E — D ← E
        case 0x54: { cpu->D = cpu->H; return 4; }                                    // LD D,H — D ← H
        case 0x55: { cpu->D = cpu->L; return 4; }                                    // LD D,L — D ← L

        case 0x56: { cpu->D = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD D,(HL) — D ← [HL]
        case 0x57: { cpu->D = cpu->A; return 4; }                                    // LD D,A — D ← A

        // LD E,r — Charger un registre dans E (0x58-0x5F)
        case 0x58: { cpu->E = cpu->B; return 4; }                                    // LD E,B — E ← B
        case 0x59: { cpu->E = cpu->C; return 4; }                                    // LD E,C — E ← C
        case 0x5A: { cpu->E = cpu->D; return 4; }                                    // LD E,D — E ← D
        case 0x5B: return 4;                                                          // LD E,E — E ← E (NOP)
        case 0x5C: { cpu->E = cpu->H; return 4; }                                    // LD E,H — E ← H
        case 0x5D: { cpu->E = cpu->L; return 4; }                                    // LD E,L — E ← L

        case 0x5E: { cpu->E = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD E,(HL) — E ← [HL]
        case 0x5F: { cpu->E = cpu->A; return 4; }                                    // LD E,A — E ← A

        // LD H,r — Charger un registre dans H (0x60-0x67)
        case 0x60: { cpu->H = cpu->B; return 4; }                                    // LD H,B — H ← B
        case 0x61: { cpu->H = cpu->C; return 4; }                                    // LD H,C — H ← C
        case 0x62: { cpu->H = cpu->D; return 4; }                                    // LD H,D — H ← D
        case 0x63: { cpu->H = cpu->E; return 4; }                                    // LD H,E — H ← E
        case 0x64: return 4;                                                          // LD H,H — H ← H (NOP)
        case 0x65: { cpu->H = cpu->L; return 4; }                                    // LD H,L — H ← L

        case 0x66: { cpu->H = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD H,(HL) — H ← [HL]
        case 0x67: { cpu->H = cpu->A; return 4; }                                    // LD H,A — H ← A

        // LD L,r — Charger un registre dans L (0x68-0x6F)
        case 0x68: { cpu->L = cpu->B; return 4; }                                    // LD L,B — L ← B
        case 0x69: { cpu->L = cpu->C; return 4; }                                    // LD L,C — L ← C
        case 0x6A: { cpu->L = cpu->D; return 4; }                                    // LD L,D — L ← D
        case 0x6B: { cpu->L = cpu->E; return 4; }                                    // LD L,E — L ← E
        case 0x6C: { cpu->L = cpu->H; return 4; }                                    // LD L,H — L ← H
        case 0x6D: return 4;                                                          // LD L,L — L ← L (NOP)

        case 0x6E: { cpu->L = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD L,(HL) — L ← [HL]
        case 0x6F: { cpu->L = cpu->A; return 4; }                                    // LD L,A — L ← A

        // LD (HL),r — Stocker un registre en mémoire [HL] (0x70-0x75)
        // Timing : 7 T-states pour accès mémoire (plus lent que les registres)
        case 0x70: { cpu->mem_write_fn(cpu->HL, cpu->B); return 7; }                 // LD (HL),B — [HL] ← B
        case 0x71: { cpu->mem_write_fn(cpu->HL, cpu->C); return 7; }                 // LD (HL),C — [HL] ← C
        case 0x72: { cpu->mem_write_fn(cpu->HL, cpu->D); return 7; }                 // LD (HL),D — [HL] ← D
        case 0x73: { cpu->mem_write_fn(cpu->HL, cpu->E); return 7; }                 // LD (HL),E — [HL] ← E
        case 0x74: { cpu->mem_write_fn(cpu->HL, cpu->H); return 7; }                 // LD (HL),H — [HL] ← H
        case 0x75: { cpu->mem_write_fn(cpu->HL, cpu->L); return 7; }                 // LD (HL),L — [HL] ← L

        // 0x76 = HALT (géré plus bas dans cette fonction)
        case 0x77: { cpu->mem_write_fn(cpu->HL, cpu->A); return 7; }                 // LD (HL),A — [HL] ← A

        // LD A,r — Charger un registre dans A (0x78-0x7F)
        case 0x78: { cpu->A = cpu->B; return 4; }                                    // LD A,B — A ← B
        case 0x79: { cpu->A = cpu->C; return 4; }                                    // LD A,C — A ← C
        case 0x7A: { cpu->A = cpu->D; return 4; }                                    // LD A,D — A ← D
        case 0x7B: { cpu->A = cpu->E; return 4; }                                    // LD A,E — A ← E
        case 0x7C: { cpu->A = cpu->H; return 4; }                                    // LD A,H — A ← H
        case 0x7D: { cpu->A = cpu->L; return 4; }                                    // LD A,L — A ← L

        case 0x7E: { cpu->A = cpu->mem_read_fn(cpu->HL); return 7; }                 // LD A,(HL) — A ← [HL]
        case 0x7F: return 4;                                                          // LD A,A — A ← A (NOP)

        // ====================================================================
        // LD nn,rp / ADD HL,rp / LD (nn),rp / LD rp,(nn) — Instructions 16-bit
        // ====================================================================
        // Chargement de mots 16-bit dans les registres pairs et opérations sur HL.

        case 0x01: { cpu->BC = z80_fetch_word(cpu); return 10; }   // LD BC,nn — Charger mot 16-bit dans BC
        case 0x11: { cpu->DE = z80_fetch_word(cpu); return 10; }   // LD DE,nn — Charger mot 16-bit dans DE
        case 0x21: { cpu->HL = z80_fetch_word(cpu); return 10; }   // LD HL,nn — Charger mot 16-bit dans HL
        case 0x31: { cpu->SP = z80_fetch_word(cpu); return 10; }   // LD SP,nn — Charger mot 16-bit dans SP

        // ADD HL,rp — Additionner un registre pair à HL (timing : 11 T-states)
        case 0x09: { uint32_t r = static_cast<uint32_t>(cpu->HL) + cpu->BC; bool c = r > 0xFFFF; bool h = ((cpu->HL & 0xFFF) + (cpu->BC & 0xFFF)) > 0xFFF;
            cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV); if (c) cpu->F |= FLAG_C; if (h) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(r >> 8)); cpu->HL = static_cast<uint16_t>(r); return 11; } // ADD HL,BC

        case 0x19: { uint32_t r = static_cast<uint32_t>(cpu->HL) + cpu->DE; bool c = r > 0xFFFF; bool h = ((cpu->HL & 0xFFF) + (cpu->DE & 0xFFF)) > 0xFFF;
            cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV); if (c) cpu->F |= FLAG_C; if (h) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(r >> 8)); cpu->HL = static_cast<uint16_t>(r); return 11; } // ADD HL,DE

        case 0x29: { uint32_t r = static_cast<uint32_t>(cpu->HL) + static_cast<uint32_t>(cpu->HL); bool c = r > 0xFFFF; bool h = ((cpu->HL & 0xFFF) + (cpu->HL & 0xFFF)) > 0xFFF;
            cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV); if (c) cpu->F |= FLAG_C; if (h) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(r >> 8)); cpu->HL = static_cast<uint16_t>(r); return 11; } // ADD HL,HL

        case 0x39: { uint32_t r = static_cast<uint32_t>(cpu->HL) + cpu->SP; bool c = r > 0xFFFF; bool h = ((cpu->HL & 0xFFF) + (cpu->SP & 0xFFF)) > 0xFFF;
            cpu->F &= (FLAG_S | FLAG_Z | FLAG_PV); if (c) cpu->F |= FLAG_C; if (h) cpu->F |= FLAG_H;
            z80_set_flags_xy(cpu, static_cast<uint8_t>(r >> 8)); cpu->HL = static_cast<uint16_t>(r); return 11; } // ADD HL,SP

        case 0x22: { // LD (nn),HL — Stocker HL en adresse nn (little-endian)
            uint16_t addr = z80_fetch_word(cpu);                           // Lire l'adresse 16-bit
            cpu->mem_write_fn(addr, static_cast<uint8_t>(cpu->HL & 0xFF)); // Écrire octet bas à addr
            cpu->mem_write_fn(addr + 1, static_cast<uint8_t>((cpu->HL >> 8) & 0xFF)); // Écrire octet haut à addr+1
            return 16;                                                     // 16 T-states pour accès mémoire 16-bit
        } // LD (nn),HL

        case 0x2A: { // LD HL,(nn) — Charger HL depuis adresse nn (little-endian)
            uint16_t addr = z80_fetch_word(cpu);                           // Lire l'adresse 16-bit
            uint8_t lo = cpu->mem_read_fn(addr);                           // Lire octet bas à addr
            uint8_t hi = cpu->mem_read_fn(addr + 1);                       // Lire octet haut à addr+1
            cpu->HL = static_cast<uint16_t>((hi << 8) | lo);              // Reconstruire HL (little-endian)
            return 16;                                                     // 16 T-states pour accès mémoire 16-bit
        } // LD HL,(nn)

        case 0x23: { cpu->HL++; return 6; }   // INC HL — Incrémenter HL de 1
        case 0x2B: { cpu->HL--; return 6; }   // DEC HL — Décrémenter HL de 1

        // ====================================================================
        // LD (BC),A / LD A,(BC) / LD (DE),A / LD A,(DE) — Accès via BC/DE
        // ====================================================================
        // Transferts entre A et la mémoire pointée par BC ou DE.

        case 0x02: { cpu->mem_write_fn(cpu->BC, cpu->A); cpu->WZ = static_cast<uint16_t>(cpu->BC + 1); return 7; }   // LD (BC),A — [BC] ← A
        case 0x0A: { cpu->A = cpu->mem_read_fn(cpu->BC); cpu->WZ = static_cast<uint16_t>(cpu->BC + 1); return 7; }   // LD A,(BC) — A ← [BC]

        case 0x12: { cpu->mem_write_fn(cpu->DE, cpu->A); cpu->WZ = static_cast<uint16_t>(cpu->DE + 1); return 7; }   // LD (DE),A — [DE] ← A
        case 0x1A: { cpu->A = cpu->mem_read_fn(cpu->DE); cpu->WZ = static_cast<uint16_t>(cpu->DE + 1); return 7; }   // LD A,(DE) — A ← [DE]

        // ====================================================================
        // LD (nn),A / LD A,(nn) — Accès mémoire via adresse directe
        // ====================================================================
        case 0x32: { uint16_t addr = z80_fetch_word(cpu); cpu->mem_write_fn(addr, cpu->A); return 13; }   // LD (nn),A — [nn] ← A
        case 0x3A: { uint16_t addr = z80_fetch_word(cpu); cpu->A = cpu->mem_read_fn(addr); cpu->WZ = static_cast<uint16_t>(addr + 1); return 13; }   // LD A,(nn) — A ← [nn]

        // ====================================================================
        // INC/DEC SP / BC / DE — Manipulation des registres 16-bit
        // ====================================================================
        case 0x03: { cpu->BC++; return 6; }   // INC BC — Incrémenter BC de 1
        case 0x0B: { cpu->BC--; return 6; }   // DEC BC — Décrémenter BC de 1
        case 0x13: { cpu->DE++; return 6; }   // INC DE — Incrémenter DE de 1
        case 0x1B: { cpu->DE--; return 6; }   // DEC DE — Décrémenter DE de 1
        case 0x3B: { cpu->SP--; return 6; }   // DEC SP — Décrémenter SP de 1
        case 0x33: { cpu->SP++; return 6; }   // INC SP — Incrémenter SP de 1

        // ====================================================================
        // LD SP,HL — Charger SP depuis HL (opcode F9)
        // ====================================================================
        case 0xF9: { cpu->SP = cpu->HL; return 6; }   // LD SP,HL — SP ← HL

        // ====================================================================
        // PUSH/POP — Opérations de pile
        // ====================================================================
        // PUSH : SP-=2, écrire haut puis bas. POP : lire bas puis haut, SP+=2.

        case 0xC5: { z80_push_word(cpu, cpu->BC); return 11; }   // PUSH BC — Empiler BC sur la pile
        case 0xD5: { z80_push_word(cpu, cpu->DE); return 11; }   // PUSH DE — Empiler DE sur la pile
        case 0xE5: { z80_push_word(cpu, cpu->HL); return 11; }   // PUSH HL — Empiler HL sur la pile
        case 0xF5: { z80_push_word(cpu, cpu->AF); return 11; }   // PUSH AF — Empiler AF (A + Flags) sur la pile

        case 0xC1: { cpu->BC = z80_pop_word(cpu); return 10; }   // POP BC — Dépiler vers BC
        case 0xD1: { cpu->DE = z80_pop_word(cpu); return 10; }   // POP DE — Dépiler vers DE
        case 0xE1: { cpu->HL = z80_pop_word(cpu); return 10; }   // POP HL — Dépiler vers HL

        // POP AF restaure A ET F (pas seulement F) — le mot dépillé est stocké dans AF complet
        case 0xF1: { cpu->AF = z80_pop_word(cpu); return 10; }   // POP AF — Dépiler vers AF (A + Flags)

        // ====================================================================
        // RST — Vecteurs de restart (sauts fixes après PUSH PC)
        // ====================================================================
        // Chaque RST empile PC sur la pile puis saute vers une adresse fixe.
        // 16 T-states pour tous les RST : 10 pour PUSH + 6 pour le saut.

        case 0xC7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0000; cpu->WZ = 0x0000; return 16; }   // RST 00 — Saut vers 0x0000
        case 0xCF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0008; cpu->WZ = 0x0008; return 16; }   // RST 08 — Saut vers 0x0008
        case 0xD7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0010; cpu->WZ = 0x0010; return 16; }   // RST 10 — Saut vers 0x0010
        case 0xDF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0018; cpu->WZ = 0x0018; return 16; }   // RST 18 — Saut vers 0x0018
        case 0xE7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0020; cpu->WZ = 0x0020; return 16; }   // RST 20 — Saut vers 0x0020
        case 0xEF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0028; cpu->WZ = 0x0028; return 16; }   // RST 28 — Saut vers 0x0028
        case 0xF7: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0030; cpu->WZ = 0x0030; return 16; }   // RST 30 — Saut vers 0x0030
        case 0xFF: { z80_push_word(cpu, cpu->PC); cpu->PC = 0x0038; cpu->WZ = 0x0038; return 16; }   // RST 38 — Saut vers 0x0038

        // ====================================================================
        // JP — Instructions de saut (conditionnels et inconditionnels)
        // ====================================================================
        // JP nn : saut inconditionnel. JP cc,nn : saut conditionnel selon flag.

        case 0xC3: { uint16_t addr = z80_fetch_word(cpu); cpu->PC = addr; cpu->WZ = addr; return 10; }   // JP nn — Saut inconditionnel
        case 0xE9: { cpu->PC = cpu->HL; cpu->WZ = cpu->HL; return 4; }   // JP (HL) — Saut vers l'adresse dans HL

        // JP conditionnels : JP cc,nn — 10 T-states si non pris, 7 si pris
        case 0xC2: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = t; } return 10; } // JP NZ,nn — Saut si Z=0

        case 0xCA: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = t; } return 10; } // JP Z,nn — Saut si Z=1
        case 0xD2: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC = t; } return 10; } // JP NC,nn — Saut si C=0
        case 0xDA: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC = t; } return 10; } // JP C,nn — Saut si C=1
        case 0xE2: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_PV)) { cpu->PC = t; } return 10; } // JP PO,nn — Saut si PV=0
        case 0xEA: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_PV)) { cpu->PC = t; } return 10; } // JP PE,nn — Saut si PV=1
        case 0xF2: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_S)) { cpu->PC = t; } return 10; } // JP P,nn — Saut si S=0
        case 0xFA: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_S)) { cpu->PC = t; } return 10; } // JP M,nn — Saut si S=1

        // ====================================================================
        // JR — Sauts relatifs conditionnels
        // ====================================================================
        // JR d : saut inconditionnel relatif. JR cc,d : saut conditionnel relatif.
        // Timing : 12 T-states si pris, 7 si non pris (sauf JR d = 12 toujours).

        case 0x18: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); cpu->PC += d; cpu->WZ = cpu->PC; return 12; }   // JR d — Saut relatif inconditionnel (12 T-states)

        case 0x20: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR NZ,d — Saut si Z=0

        case 0x28: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR Z,d — Saut si Z=1
        case 0x30: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR NC,d — Saut si C=0
        case 0x38: { int8_t d = static_cast<int8_t>(z80_fetch_byte(cpu)); if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC += d; cpu->WZ = cpu->PC; return 12; } return 7; }   // JR C,d — Saut si C=1

        // ====================================================================
        // CALL — Appels de sous-routine conditionnels
        // ====================================================================
        // CALL nn : appel inconditionnel (17 T-states). CALL cc,nn : 10 si non pris, 17 si pris.

        case 0xCD: { uint16_t addr = z80_fetch_word(cpu); z80_push_word(cpu, cpu->PC); cpu->PC = addr; cpu->WZ = addr; return 17; }   // CALL nn — Appel inconditionnel (PUSH PC + JP)

        case 0xC4: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_Z)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL NZ,nn — Appeler si Z=0

        case 0xCC: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_Z)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL Z,nn — Appeler si Z=1
        case 0xD4: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_C)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL NC,nn — Appeler si C=0
        case 0xDC: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_C)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL C,nn — Appeler si C=1
        case 0xE4: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_PV)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL PO,nn — Appeler si PV=0
        case 0xEC: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_PV)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL PE,nn — Appeler si PV=1
        case 0xF4: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (!TEST_FLAG(cpu, FLAG_S)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL P,nn — Appeler si S=0
        case 0xFC: { uint16_t t = z80_fetch_word(cpu); cpu->WZ = t; if (TEST_FLAG(cpu, FLAG_S)) { z80_push_word(cpu, cpu->PC); cpu->PC = t; return 17; } return 10; } // CALL M,nn — Appeler si S=1

        // ====================================================================
        // RET — Retours de sous-routine conditionnels
        // ====================================================================
        // RET : retour inconditionnel (10 T-states). RET cc : 5 si non pris, 11 si pris.

        case 0xC9: { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 10; }   // RET — Retour inconditionnel (POP PC)

        case 0xC0: { if (!TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET NZ — Retour si Z=0

        case 0xC8: { if (TEST_FLAG(cpu, FLAG_Z)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET Z — Retour si Z=1
        case 0xD0: { if (!TEST_FLAG(cpu, FLAG_C)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET NC — Retour si C=0
        case 0xD8: { if (TEST_FLAG(cpu, FLAG_C)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; }   // RET C — Retour si C=1

        case 0xE0: { if (!TEST_FLAG(cpu, FLAG_PV)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; } // RET PO — Retour si PV=0
        case 0xE8: { if (TEST_FLAG(cpu, FLAG_PV))  { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; } // RET PE — Retour si PV=1
        case 0xF0: { if (!TEST_FLAG(cpu, FLAG_S)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; } // RET P — Retour si S=0
        case 0xF8: { if (TEST_FLAG(cpu, FLAG_S)) { cpu->PC = z80_pop_word(cpu); cpu->WZ = cpu->PC; return 11; } return 5; } // RET M — Retour si S=1

        // ====================================================================
        // EX — Instructions d'échange
        // ====================================================================

        case 0xE3: { /* EX (SP),HL — Échanger HL avec le sommet de la pile */
            uint16_t val = cpu->HL;                              // Sauvegarder HL
            uint8_t lo = cpu->mem_read_fn(cpu->SP);              // Lire octet bas de [SP]
            uint8_t hi = cpu->mem_read_fn(cpu->SP + 1);          // Lire octet haut de [SP+1]
            cpu->HL = static_cast<uint16_t>((hi << 8) | lo);     // HL ← [SP] (mot dépillé)
            cpu->mem_write_fn(cpu->SP, static_cast<uint8_t>(val & 0xFF));       // [SP] ← octet bas de HL
            cpu->mem_write_fn(cpu->SP + 1, static_cast<uint8_t>((val >> 8) & 0xFF)); // [SP+1] ← octet haut de HL
            cpu->WZ = cpu->HL;                                   // Mettre à jour WZ (MEMPTR)
            return 19; }                                          // 19 T-states pour EX (SP),HL

        // ====================================================================
        // Flags — DAA, CPL, SCF, CCF
        // ====================================================================
        // Instructions spéciales qui modifient l'accumulateur ou les flags.

        case 0x3F: { /* CCF — Complement Carry Flag : inverser le flag Carry */
            bool old_carry = TEST_FLAG(cpu, FLAG_C) != 0;       // Sauvegarder l'état actuel de C
            cpu->F &= ~(FLAG_H | FLAG_N | FLAG_C);              // Effacer H, N, C
            if (old_carry) cpu->F |= FLAG_H;                    // Si C=1 → H=1 après CCF
            if (!old_carry) cpu->F |= FLAG_C;                   // Si C=0 → C=1 après CCF
            z80_set_flags_xy(cpu, cpu->A);                     // Flags X/Y copiés depuis A
            return 4; }                                          // 4 T-states

        case 0x27: { /* DAA — Decimal Adjust Accumulator : ajustement BCD */
            // Après une addition (ADD/ADC), DAA ajuste A pour qu'il représente
            // un nombre BCD (Binary-Coded Decimal) à deux chiffres.
            uint8_t a = cpu->A;
            uint8_t correction = 0;
            bool new_carry = false;

            // Si demi-carry ou nibble bas > 9, ajouter 6 pour corriger
            if (TEST_FLAG(cpu, FLAG_H) || (!TEST_FLAG(cpu, FLAG_N) && (a & 0xF) > 9))
                correction |= 0x06;

            // Si carry ou valeur > 0x99, ajouter 0x60 pour corriger le nibble haut
            if (TEST_FLAG(cpu, FLAG_C) || (!TEST_FLAG(cpu, FLAG_N) && a > 0x99)) {
                correction |= 0x60;
                new_carry = true;
            }

            // Appliquer la correction : ajouter si ADD, soustraire si SUB
            a += TEST_FLAG(cpu, FLAG_N) ? -static_cast<int8_t>(correction) : static_cast<uint8_t>(correction);

            // Positionner les flags selon le résultat DAA
            cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_C | FLAG_X | FLAG_Y);
            if (a & 0x80)    cpu->F |= FLAG_S;                 // Sign flag
            if (a == 0)      cpu->F |= FLAG_Z;                 // Zero flag
            if (new_carry)   cpu->F |= FLAG_C;                 // Carry flag
            if (z80_parity(a)) cpu->F |= FLAG_PV;              // Parity flag
            z80_set_flags_xy(cpu, a);                         // Flags X/Y undocumented
            cpu->A = a;                                        // Stocker le résultat ajusté
            return 4; }                                          // 4 T-states

        case 0x2F: { /* CPL — Complement Accumulator : inversion de tous les bits de A */
            cpu->A = static_cast<uint8_t>(~cpu->A);            // Inversion bit à bit
            cpu->F |= FLAG_H | FLAG_N;                         // H=1, N=1 (S/Z/PV/C/X/Y préservés)
            z80_set_flags_xy(cpu, cpu->A);                   // Flags X/Y copiés depuis A
            return 4; }                                          // 4 T-states

        case 0x37: { /* SCF — Set Carry Flag : positionner C=1, effacer H et N */
            cpu->F &= ~(FLAG_H | FLAG_N | FLAG_C);              // Effacer H, N, C uniquement (S/Z/PV préservés)
            cpu->F |= FLAG_C;                                   // C=1
            z80_set_flags_xy(cpu, cpu->A);                   // Flags X/Y copiés depuis A
            return 4; }                                          // 4 T-states

        // ====================================================================
        // Interrupts — EI (Enable Interrupts) et DI (Disable Interrupts)
        // ====================================================================

        case 0xFB: { /* EI — Enable Interrupts : activer les interruptions avec délai d'une instruction */
            // Sur Z80 réel, EI active IFF1/IFF2 seulement après l'instruction suivante.
            cpu->ei_delay = true;                               // IFF1/IFF2 activés en fin de z80_step()
#ifdef GALAXIAN_DEBUG_IM2
            printf("[IM2-SETUP] PC=%04X FB EI\n", cpu->PC - 1);
#endif
            return 4; }                                          // 4 T-states

        case 0xF3: { /* DI — Disable Interrupts : désactiver les interruptions */
            cpu->IFF1 = false;                                  // Désactiver IFF1
            cpu->IFF2 = false;                                  // Désactiver IFF2
            return 4; }                                          // 4 T-states

        // ====================================================================
        // EX AF,AF' / EXX — Échange de registres alternatifs
        // ====================================================================

        case 0x08: { /* EX AF,AF' — Échanger AF avec AF' (registres alternatifs) */
            uint16_t tmp = cpu->AF;                             // Sauvegarder AF
            cpu->AF = cpu->AF_;                                 // AF ← AF'
            cpu->AF_ = tmp;                                     // AF' ← ancien AF
            return 4; }                                          // 4 T-states

        case 0xD9: { /* EXX — Exchange Register Set : échanger BC/DE/HL avec leurs alternatifs */
            uint16_t tmprp = cpu->BC;                           // Échanger BC ↔ BC'
            cpu->BC = cpu->BC_;
            cpu->BC_ = tmprp;
            tmprp = cpu->DE;                                    // Échanger DE ↔ DE'
            cpu->DE = cpu->DE_;
            cpu->DE_ = tmprp;
            tmprp = cpu->HL;                                    // Échanger HL ↔ HL'
            cpu->HL = cpu->HL_;
            cpu->HL_ = tmprp;
            return 4; }                                          // 4 T-states

        // ====================================================================
        // HALT — Mettre le CPU en pause
        // ====================================================================

        case 0x76: { /* HALT — Mettre le CPU en état d'attente */
            cpu->halted = true;                                 // Activer l'état HALT
            // PC n'est PAS décrémenté : le fetch a déjà avancé PC au-delà de l'opcode HALT,
            // c'est exactement le comportement réel du Z80 (le PC poussé par un INT = HALT+1)
            return 4; }                                          // 4 T-states

        // ====================================================================
        // IN/OUT — Entrées/Sorties I/O avec octet immédiat
        // ====================================================================
        // IN A,(n) : lire depuis port (A<<8 | n). OUT (n),A : écrire vers port.

        case 0xDB: { /* IN A,(n) — Lire un octet depuis un port I/O */
            uint8_t n = z80_fetch_byte(cpu);                    // Lire l'octet immédiat du port bas
            uint16_t port = static_cast<uint16_t>(static_cast<uint16_t>(cpu->A) << 8 | n); // Port complet = A<<8 | n
            cpu->A = cpu->io_read_fn(port);                     // Lire le port via le callback I/O de l'hôte

            // En IM 2, mettre à jour WZ avec BC+1 (comportement réel du Z80)
            if (cpu->IM == 2) cpu->WZ = cpu->BC + 1;

            // Note : IN A,(n) ne modifie AUCUN flag sur le Z80.
            // Seules les instructions de type IN r,(C) affectent les flags.
            return 12; }                                         // 12 T-states pour accès I/O

        case 0xD3: { /* OUT (n),A — Écrire A vers un port I/O */
            uint8_t n = z80_fetch_byte(cpu);                    // Lire l'octet immédiat du port bas
            uint16_t port = static_cast<uint16_t>(static_cast<uint16_t>(cpu->A) << 8 | n); // Port complet = A<<8 | n
            cpu->io_write_fn(port, cpu->A);                    // Écrire le port via le callback I/O de l'hôte

            if (cpu->IM == 2) cpu->WZ = cpu->BC + 1;           // Mettre à jour WZ en IM 2
            return 12; }                                         // 12 T-states pour accès I/O

        // ====================================================================
        // Préfixes spéciaux — CB / DD / ED / FD
        // ====================================================================
        // Ces opcodes nécessitent un deuxième octet après le préfixe.
        // Le dispatcher lit le second opcode et appelle le sous-dispatcher approprié.

        case 0xCB: { /* Préfixe CB — Instructions sur registres/mémoire avec décalage */
            return z80_exec_cb(cpu);                        // Dispatcher CB (RLC/RRC/RL/RR/BIT/RES/SET)
        }

        case 0xDD: { /* Préfixe DD — Registre d'index IX (substitue HL) */
            return z80_exec_dd(cpu);                        // Dispatcher DD (IX instructions)
        }

        case 0xED: { /* Préfixe ED — Instructions spécialisées 16-bit */
            return z80_exec_ed(cpu);                        // Dispatcher ED (ADC/SBC HL, Block I/O, LD I/A/R)
        }

        case 0xFD: { /* Préfixe FD — Registre d'index IY (substitue HL) */
            return z80_exec_fd(cpu);                        // Dispatcher FD (IY instructions)
        }

        // ====================================================================
        // Default : NOP — Opcode non implémenté traité comme NOP
        // ====================================================================
        default: return 4;   // 4 T-states pour opcode inconnu (traité comme NOP)
    }
}