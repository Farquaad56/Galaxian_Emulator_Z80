#include "../include/cpu/z80.h"
#include "../include/cpu/z80_cb.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Préfixe CB Implementation
// ============================================================================
//
// Ce fichier implémente toutes les instructions du préfixe CB du Z80 :
//   - Rotations : RLC, RRC, RL, RR, SLA, SRA, SLL, SRL (x=0)
//   - BIT b,r   : Test de bit sans modifier le registre (x=1)
//   - RES b,r   : Reset d'un bit — bit mis à 0 (x=2)
//   - SET b,r   : Set d'un bit — bit mis à 1 (x=3)
//
// Chaque instruction est précédée du préfixe CB, puis de l'opcode qui contient :
//   x (2 bits hauts) → type d'opération
//   y (3 bits moyens) → numéro du bit (0-7)
//   z (3 bits bas) → index registre (0=B, 1=C, ..., 6=(HL), 7=A)
//
// Référence : Zilog Z80 CPU User's Manual — Section "CB Prefix Instructions"
//            Sean Young — "Z80 Undocumented Instructions" (§ SLL = undocumented)
// ============================================================================

// ============================================================================
// Helpers d'accès registre (locaux au préfixe CB)
// ============================================================================
// Ces fonctions sont identiques à z80_read_r/z80_write_r mais locales ici
// car le dispatcher CB peut être utilisé avec des registres substitués
// (IXH/IXL pour DD, IYH/IYL pour FD via z80_ddcb.cpp).

/**
 * @brief Lit la valeur d'un registre 8-bit par index.
 * @param cpu  Pointeur vers la structure Z80
 * @param idx  Index registre (0=B, 1=C, ..., 6=(HL), 7=A)
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
 * @param idx     Index registre (0=B, 1=C, ..., 6=(HL), 7=A)
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
// Rotations/Décalages (x=0) — RLC, RRC, RL, RR, SLA, SRA, SLL, SRL
// ============================================================================
// Chaque fonction prend une valeur 8-bit, applique la rotation/décalage,
// positionne les flags S, Z, C, PV, X/Y, et retourne le résultat.

/**
 * @brief RLC — Rotate Left Circular (Rotation circulaire gauche)
 * Le bit 7 est déplacé vers le bit 0 ET vers le flag Carry.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à rotationner
 * @return     Résultat de la rotation circulaire gauche
 */
static uint8_t rot_rlc(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 0x80) != 0;           // Sauvegarder le bit 7 avant rotation
    uint8_t result = static_cast<uint8_t>(((val << 1) | (carry_out ? 1 : 0)) & 0xFF);

    // Positionner les flags selon le résultat RLC
    cpu->F = 0;                                   // Effacer tous les flags
    if (result == 0) cpu->F |= FLAG_Z;            // Zero flag
    if (result & 0x80) cpu->F |= FLAG_S;          // Sign flag
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 7 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;   // Parity flag
    z80_set_flags_xy(cpu, result);                // Flags X/Y undocumented
    return result;
}

/**
 * @brief RRC — Rotate Right Circular (Rotation circulaire droite)
 * Le bit 0 est déplacé vers le bit 7 ET vers le flag Carry.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à rotationner
 * @return     Résultat de la rotation circulaire droite
 */
static uint8_t rot_rrc(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 1) != 0;              // Sauvegarder le bit 0 avant rotation
    uint8_t result = static_cast<uint8_t>((val >> 1) | (carry_out ? 0x80 : 0));

    // Positionner les flags selon le résultat RRC
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief RL — Rotate Left through Carry (Rotation gauche via Carry)
 * Le bit 7 est déplacé vers le bit 0. Le flag Carry actuel va dans le bit 0.
 * L'ancien bit 7 devient le nouveau Carry.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à rotationner
 * @return     Résultat de la rotation gauche via Carry
 */
static uint8_t rot_rl(Z80* cpu, uint8_t val) {
    bool carry_in = (cpu->F & FLAG_C) != 0;       // Récupérer le flag Carry actuel
    bool carry_out = (val & 0x80) != 0;           // Sauvegarder le bit 7 avant rotation
    uint8_t result = static_cast<uint8_t>(((val << 1) | (carry_in ? 1 : 0)) & 0xFF);

    // Positionner les flags selon le résultat RL
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 7 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief RR — Rotate Right through Carry (Rotation droite via Carry)
 * Le bit 0 est déplacé vers le bit 7. Le flag Carry actuel va dans le bit 7.
 * L'ancien bit 0 devient le nouveau Carry.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à rotationner
 * @return     Résultat de la rotation droite via Carry
 */
static uint8_t rot_rr(Z80* cpu, uint8_t val) {
    bool carry_in = (cpu->F & FLAG_C) != 0;       // Récupérer le flag Carry actuel
    bool carry_out = (val & 1) != 0;              // Sauvegarder le bit 0 avant rotation
    uint8_t result = static_cast<uint8_t>((val >> 1) | (carry_in ? 0x80 : 0));

    // Positionner les flags selon le résultat RR
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 0 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief SLA — Shift Left Arithmetic (Décalage arithmétique gauche)
 * Décale tous les bits d'un cran vers la gauche. Le bit 0 est mis à 0.
 * Le bit 7 va dans Carry. Équivalent à RLC mais bit 0 = 0 et sans circularité.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à décaler
 * @return     Résultat du décalage arithmétique gauche
 */
static uint8_t rot_sla(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 0x80) != 0;           // Sauvegarder le bit 7 avant décalage
    uint8_t result = static_cast<uint8_t>((val << 1) & 0xFF); // Bit 0 mis à 0

    // Positionner les flags selon le résultat SLA
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 7 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief SRA — Shift Right Arithmetic (Décalage arithmétique droit)
 * Décale tous les bits d'un cran vers la droite. Le bit 7 (signe) est conservé.
 * Le bit 0 va dans Carry. Permet de décaler un nombre signé sans changer son signe.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à décaler
 * @return     Résultat du décalage arithmétique droit
 */
static uint8_t rot_sra(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 1) != 0;              // Sauvegarder le bit 0 avant décalage
    uint8_t msb = val & 0x80;                     // Préserver le bit de signe
    uint8_t result = static_cast<uint8_t>((val >> 1) | msb); // Bit 7 conservé

    // Positionner les flags selon le résultat SRA
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 0 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief SLL — Shift Left Logical (Décalage logique gauche undocumented)
 * Décale tous les bits d'un cran vers la gauche. Le bit 0 est mis à 1.
 * Cette instruction est undocumented sur le Z80 réel mais présente sur le silicium.
 * Le bit 7 va dans Carry.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à décaler
 * @return     Résultat du décalage logique gauche (undocumented)
 */
static uint8_t rot_sll(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 0x80) != 0;           // Sauvegarder le bit 7 avant décalage
    uint8_t result = static_cast<uint8_t>(((val << 1) | 0x01) & 0xFF); // Bit 0 mis à 1

    // Positionner les flags selon le résultat SLL
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 7 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

/**
 * @brief SRL — Shift Right Logical (Décalage logique droit)
 * Décale tous les bits d'un cran vers la droite. Le bit 7 est mis à 0.
 * Le bit 0 va dans Carry. Cette instruction est documentée officiellement.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur 8-bit à décaler
 * @return     Résultat du décalage logique droit
 */
static uint8_t rot_srl(Z80* cpu, uint8_t val) {
    bool carry_out = (val & 1) != 0;              // Sauvegarder le bit 0 avant décalage
    uint8_t result = static_cast<uint8_t>(val >> 1); // Bit 7 mis à 0

    // Positionner les flags selon le résultat SRL
    cpu->F = 0;
    if (result == 0) cpu->F |= FLAG_Z;
    if (result & 0x80) cpu->F |= FLAG_S;
    cpu->F |= carry_out ? FLAG_C : 0;             // Carry = bit 0 original
    cpu->F |= z80_parity(result) ? FLAG_PV : 0;
    z80_set_flags_xy(cpu, result);
    return result;
}

// ============================================================================
// Dispatcher de rotations — applique la rotation selon le champ y (3 bits)
// ============================================================================
// Le champ y (bits 3-5 de l'opcode CB) détermine le type de rotation :
//   y=0 → RLC, y=1 → RRC, y=2 → RL, y=3 → RR
//   y=4 → SLA, y=5 → SRA, y=6 → SLL (undoc), y=7 → SRL

/**
 * @brief Applique la rotation/décalage selon le type y.
 * @param cpu  Pointeur vers la structure Z80
 * @param y    Type de rotation (0-7)
 * @param val  Valeur 8-bit à transformer
 * @return     Résultat de l'opération de rotation/décalage
 */
static uint8_t apply_rot(Z80* cpu, int y, uint8_t val) {
    switch (y) {
        case 0: return rot_rlc(cpu, val);   // y=0 → RLC — Rotate Left Circular
        case 1: return rot_rrc(cpu, val);   // y=1 → RRC — Rotate Right Circular
        case 2: return rot_rl(cpu, val);    // y=2 → RL — Rotate Left through Carry
        case 3: return rot_rr(cpu, val);    // y=3 → RR — Rotate Right through Carry
        case 4: return rot_sla(cpu, val);   // y=4 → SLA — Shift Left Arithmetic
        case 5: return rot_sra(cpu, val);   // y=5 → SRA — Shift Right Arithmetic
        case 6: return rot_sll(cpu, val);   // y=6 → SLL — Shift Left Logical (undocumented)
        case 7: return rot_srl(cpu, val);   // y=7 → SRL — Shift Right Logical
        default: return 0;                  // Valeur par défaut pour y invalide
    }
}

// ============================================================================
// RRCA / RLCA — Rotations accumulateur sans préfixe CB
// ============================================================================
// Ces instructions sont exécutées directement par leur opcode (0x0F et 0x07)
// dans le décodeur principal, mais les implémentations sont regroupées ici.

/**
 * @brief RRCA — Rotate Accumulator Right (Rotation circulaire droite de A)
 * Décale tous les bits de A d'un cran vers la droite. Le bit 0 va dans Carry.
 * Le bit 7 reçoit le bit 0 original. Flags : S=0, Z=0, H=0, PV=0, C=bit0_original.
 * NOTE : Sur le Z80 réel, le flag C est positionné mais pas effacé avant.
 * @param cpu  Pointeur vers la structure Z80
 */
void z80_rot_rrca(Z80* cpu) {
    bool carry_out = (cpu->A & 1) != 0;           // Sauvegarder le bit 0 de A
    uint8_t result = static_cast<uint8_t>((cpu->A >> 1) | (carry_out ? 0x80 : 0));

    // Effacer les flags S, Z, H, PV, X, Y — préserver C sur certains Z80
    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y);
    // Positionner Carry = bit 0 original de A
    cpu->F |= carry_out ? FLAG_C : 0;
    z80_set_flags_xy(cpu, result);                // Flags X/Y copiés depuis le résultat
    cpu->A = result;                              // Stocker le résultat dans A
}

/**
 * @brief RLCA — Rotate Accumulator Left (Rotation circulaire gauche de A)
 * Décale tous les bits de A d'un cran vers la gauche. Le bit 7 va dans Carry.
 * Le bit 0 reçoit le bit 7 original. Flags : S=0, Z=0, H=0, PV=0, C=bit7_original.
 * @param cpu  Pointeur vers la structure Z80
 */
void z80_rot_rlca(Z80* cpu) {
    bool carry_out = (cpu->A & 0x80) != 0;        // Sauvegarder le bit 7 de A
    uint8_t result = static_cast<uint8_t>(((cpu->A << 1) | (carry_out ? 1 : 0)) & 0xFF);

    // Effacer les flags S, Z, H, PV, X, Y — préserver C sur certains Z80
    cpu->F &= ~(FLAG_S | FLAG_Z | FLAG_H | FLAG_PV | FLAG_X | FLAG_Y);
    // Positionner Carry = bit 7 original de A
    cpu->F |= carry_out ? FLAG_C : 0;
    z80_set_flags_xy(cpu, result);                // Flags X/Y copiés depuis le résultat
    cpu->A = result;                              // Stocker le résultat dans A
}

// ============================================================================
// BIT (x=1) — Test de bit sans modifier le registre
// ============================================================================
// BIT b,r teste le bit y du registre z sans le modifier.
// Flags positionnés : Z=(bit non set), S=(bit 7 du registre), H=1, PV=(bit non set).

/**
 * @brief BIT b,r — Tester un bit d'un registre sans le modifier.
 *
 * Teste si le bit y est positionné dans la valeur lue depuis le registre z.
 * Le registre n'est PAS modifié. Les flags sont positionnés comme suit :
 *   Z=1 si le bit est à 0, Z=0 si le bit est à 1
 *   S=1 si y==7 et le bit est à 1 (bit de signe)
 *   H=1 toujours (comportement réel du Z80)
 *   PV=1 si le bit est à 0 (comme Zero flag)
 *   X/Y = bits 3/5 de la valeur testée (ou WZ>>8 pour BIT b,(HL))
 *
 * @param cpu  Pointeur vers la structure Z80
 * @param y    Numéro du bit à tester (0-7)
 * @param val  Valeur 8-bit dans laquelle tester le bit
 * @param idx  Index registre source (0=B, ..., 6=(HL), 7=A)
 */
static void op_bit(Z80* cpu, int y, uint8_t val, int idx) {
    bool bit_set = (val & (1 << y)) != 0;         // Vérifier si le bit y est à 1

    // Préserver uniquement le flag Carry, effacer S, Z, H, N, PV, X, Y
    cpu->F &= FLAG_C;
    cpu->F |= FLAG_H;                             // H est TOUJOURS à 1 pour BIT (comportement réel)

    if (!bit_set) {
        cpu->F |= (FLAG_Z | FLAG_PV);             // Z=1 et PV=1 si bit non positionné
    }

    // Si le bit testé est le bit 7 (signe) ET qu'il est à 1, positionner S
    if (y == 7 && bit_set) {
        cpu->F |= FLAG_S;                         // Sign flag = bit 7 du registre
    }

    // Flags X et Y undocumented :
    // Pour BIT b,r (r≠(HL)) : bits 3/5 de la valeur du registre testé
    // Pour BIT b,(HL) : bits 3/5 de WZ >> 8 (octet haut de MEMPTR — comportement réel Z80)
    if (idx == 6) {
        cpu->F |= (static_cast<uint8_t>(cpu->WZ >> 8) & (FLAG_X | FLAG_Y));
    } else {
        cpu->F |= (val & (FLAG_X | FLAG_Y));      // Bits X/Y depuis la valeur testée
    }
}

// ============================================================================
// RES / SET (x=2, x=3) — Reset et Set de bits
// ============================================================================
// RES met un bit à 0, SET met un bit à 1. Aucun flag n'est modifié
// sur le Z80 réel (comportement documenté par Sean Young).

/**
 * @brief RES b,r — Reset d'un bit (mettre le bit y à 0).
 * Aucun flag n'est modifié — comportement exact du Z80 réel.
 * @param cpu     Pointeur vers la structure Z80 (non utilisé pour les flags)
 * @param y       Numéro du bit à reset (0-7)
 * @param val     Valeur 8-bit dans laquelle reset le bit
 * @return        Nouvelle valeur avec le bit y mis à 0
 */
static uint8_t op_res(Z80* /*cpu*/, int y, uint8_t val) {
    // RES : bit mis à 0 via masque — aucun flag modifié sur le Z80 réel
    return val & ~(1 << y);                       // Masquer le bit y (mettre à 0)
}

/**
 * @brief SET b,r — Set d'un bit (mettre le bit y à 1).
 * Aucun flag n'est modifié — comportement exact du Z80 réel.
 * @param cpu     Pointeur vers la structure Z80 (non utilisé pour les flags)
 * @param y       Numéro du bit à set (0-7)
 * @param val     Valeur 8-bit dans laquelle set le bit
 * @return        Nouvelle valeur avec le bit y mis à 1
 */
static uint8_t op_set(Z80* /*cpu*/, int y, uint8_t val) {
    // SET : bit mis à 1 via OR — aucun flag modifié sur le Z80 réel
    return val | (1 << y);                        // Positionner le bit y (mettre à 1)
}

// ============================================================================
// Dispatcher CB principal — z80_exec_cb
// ============================================================================
// Lit l'opcode suivant le préfixe CB, décode x/y/z et exécute l'opération.
// Retourne les T-states : 8 pour r≠(HL), 15/12 pour r=(HL).

/**
 * @brief Dispatcher du préfixe CB — Exécute une instruction CB.
 *
 * Lit l'opcode suivant le préfixe CB, décode les champs x (type), y (bit), z (reg),
 * et exécute l'opération correspondante :
 *   x=0 → Rotation/Décalage (RLC/RRC/RL/RR/SLA/SRA/SLL/SRL)
 *   x=1 → BIT b,r — Test de bit sans modifier le registre
 *   x=2 → RES b,r — Reset d'un bit (bit mis à 0)
 *   x=3 → SET b,r — Set d'un bit (bit mis à 1)
 *
 * T-states : 8 cycles pour les registres B/C/D/E/H/L/A,
 *           15/12 cycles pour l'accès mémoire [HL] (plus lent).
 *
 * @param cpu  Pointeur vers la structure Z80
 * @return     Nombre de T-states exécutés (8 ou 12/15)
 */
int z80_exec_cb(Z80* cpu) {
    // Lire l'opcode CB et décoder les champs x, y, z
    uint8_t opcode = z80_fetch_byte(cpu);         // Fetch l'opcode après préfixe CB
    uint8_t x = (opcode >> 6) & 3;                // Bits 7-6 : type d'opération (0=rot, 1=bit, 2=res, 3=set)
    uint8_t y = (opcode >> 3) & 7;                // Bits 5-3 : numéro du bit (0-7)
    uint8_t z = opcode & 7;                       // Bits 2-0 : index registre (0=B ... 7=A)

    switch (x) {
    case 0: { // Rotations/Décalages — RLC, RRC, RL, RR, SLA, SRA, SLL, SRL
        uint8_t val = read_r(cpu, z);             // Lire la valeur du registre z
        uint8_t result = apply_rot(cpu, y, val);  // Appliquer la rotation selon y
        write_r(cpu, z, result);                  // Écrire le résultat dans le registre z
        return (z == 6) ? 15 : 8;                 // 15 T-states pour [HL], 8 pour les autres registres
    }
    case 1: { // BIT — Test de bit sans modifier le registre
        uint8_t val = read_r(cpu, z);             // Lire la valeur du registre z
        op_bit(cpu, y, val, z);                   // Tester le bit y dans val
        return (z == 6) ? 12 : 8;                 // 12 T-states pour [HL], 8 pour les autres registres
    }
    case 2: { // RES — Reset d'un bit (mettre à 0)
        uint8_t val = read_r(cpu, z);             // Lire la valeur du registre z
        uint8_t result = op_res(cpu, y, val);     // Mettre le bit y à 0
        write_r(cpu, z, result);                  // Écrire le résultat dans le registre z
        return (z == 6) ? 15 : 8;                 // 15 T-states pour [HL], 8 pour les autres registres
    }
    case 3: { // SET — Set d'un bit (mettre à 1)
        uint8_t val = read_r(cpu, z);             // Lire la valeur du registre z
        uint8_t result = op_set(cpu, y, val);     // Mettre le bit y à 1
        write_r(cpu, z, result);                  // Écrire le résultat dans le registre z
        return (z == 6) ? 15 : 8;                 // 15 T-states pour [HL], 8 pour les autres registres
    }
    }
    return 8; // NOP fallback — valeur par défaut en cas d'opcode invalide
}

// ============================================================================
// Préfixe DD/FD CB — z80_exec_xycb
// ============================================================================
// Traite les instructions avec préfixe DD CB ou FD CB :
//   - Rotation/décalage de (IX/IY+d) avec résultat stocké dans la mémoire ET le registre z
//   - BIT y,(IX/IY+d) — test de bit sur la mémoire indexée
//   - RES/SET y,(IX/IY+d) — reset/set d'un bit sur la mémoire indexée
//
// Spécificité Z80 : pour les rotations, le résultat est stocké à l'adresse (IX/IY+d)
// ET dans le registre z (comportement undocumented mais présent sur silicium).
// Les flags X/Y proviennent de WZ >> 8 (octet haut de MEMPTR).
// ============================================================================

int z80_exec_xycb(Z80* cpu, uint16_t* ireg) {
    // Lire le déplacement signé 8-bit puis l'opcode CB
    int8_t  d   = static_cast<int8_t>(z80_fetch_byte(cpu));
    uint8_t op  = z80_fetch_byte(cpu);
    uint16_t addr = static_cast<uint16_t>(static_cast<int32_t>(*ireg) + static_cast<int32_t>(d));
    cpu->WZ = addr;

    // Décoder x (type), y (bit), z (register)
    uint8_t x = (op >> 6) & 3;
    uint8_t y = (op >> 3) & 7;
    uint8_t z = op & 7;

    switch (x) {
    case 0: { // Rotations / décalages sur (IX/IY+d) — résultat → mémoire ET registre z (undoc)
        uint8_t val = cpu->mem_read_fn(addr);
        uint8_t res = val;
        bool carry_out;

        switch (y) {
        case 0: // RLC (val << 1, bit7 → C et bit0)
            carry_out = (val & 0x80) != 0;
            res = static_cast<uint8_t>((val << 1) | (carry_out ? 1 : 0));
            break;
        case 1: // RRC (val >> 1, bit0 → C et bit7)
            carry_out = (val & 0x01) != 0;
            res = static_cast<uint8_t>((val >> 1) | (carry_out ? 0x80 : 0));
            break;
        case 2: // RL (val << 1, C_in → bit0, bit7 → C)
            carry_out = (val & 0x80) != 0;
            res = static_cast<uint8_t>((val << 1) | ((cpu->F & FLAG_C) ? 1 : 0));
            break;
        case 3: // RR (val >> 1, C_in → bit7, bit0 → C)
            carry_out = (val & 0x01) != 0;
            res = static_cast<uint8_t>((val >> 1) | ((cpu->F & FLAG_C) ? 0x80 : 0));
            break;
        case 4: // SLA (val << 1, bit0=0)
            carry_out = (val & 0x80) != 0;
            res = static_cast<uint8_t>((val << 1) & 0xFF);
            break;
        case 5: // SRA (val >> 1, bit7 conservé)
            carry_out = (val & 0x01) != 0;
            res = static_cast<uint8_t>((val >> 1) | (val & 0x80));
            break;
        case 6: // SLL (undocumented : val << 1, bit0=1)
            carry_out = (val & 0x80) != 0;
            res = static_cast<uint8_t>(((val << 1) | 0x01) & 0xFF);
            break;
        case 7: // SRL (val >> 1, bit7=0)
            carry_out = (val & 0x01) != 0;
            res = static_cast<uint8_t>(val >> 1);
            break;
        }

        // Positionner les flags
        cpu->F = 0;
        if (res == 0)     cpu->F |= FLAG_Z;
        if (res & 0x80)   cpu->F |= FLAG_S;
        if (carry_out)    cpu->F |= FLAG_C;
        if (z80_parity(res)) cpu->F |= FLAG_PV;
        z80_set_flags_xy(cpu, res);

        // Écrire dans la mémoire indexée
        cpu->mem_write_fn(addr, res);

        // Undocumented : écrire aussi dans le registre z (sauf si z=6 = (IX+d))
        if (z != 6) {
            switch (z) {
            case 0: cpu->B = res; break;
            case 1: cpu->C = res; break;
            case 2: cpu->D = res; break;
            case 3: cpu->E = res; break;
            case 4: cpu->H = res; break;
            case 5: cpu->L = res; break;
            case 7: cpu->A = res; break;
            }
        }

        return 23;
    }
    case 1: { // BIT y,(IX/IY+d) — test de bit sur la mémoire indexée
        uint8_t val = cpu->mem_read_fn(addr);
        bool bit_set = (val & (1 << y)) != 0;

        cpu->F &= FLAG_C;
        cpu->F |= FLAG_H;
        if (!bit_set) {
            cpu->F |= FLAG_Z | FLAG_PV;
        }
        if (y == 7 && bit_set) {
            cpu->F |= FLAG_S;
        }
        // Flags X/Y depuis le haut de l'adresse (comportement réel Z80 pour BIT b,(IX+d))
        cpu->F |= (static_cast<uint8_t>(addr >> 8) & (FLAG_X | FLAG_Y));
        return 20;
    }
    case 2: { // RES y,(IX/IY+d) — reset d'un bit
        uint8_t val = cpu->mem_read_fn(addr);
        cpu->mem_write_fn(addr, static_cast<uint8_t>(val & ~(1 << y)));
        return 23;
    }
    default: { // SET y,(IX/IY+d) — set d'un bit
        uint8_t val = cpu->mem_read_fn(addr);
        cpu->mem_write_fn(addr, static_cast<uint8_t>(val | (1 << y)));
        return 23;
    }
    }
}
