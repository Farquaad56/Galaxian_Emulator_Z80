#ifndef Z80_CB_H
#define Z80_CB_H

#include "z80.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Préfixe CB Header
// ============================================================================
//
// Ce fichier définit les fonctions pour le dispatcher du préfixe CB et les
// rotations accumulateur sans préfixe (RLCA, RRCA).
//
// Le préfixe CB précède un opcode et applique des opérations de rotation,
// bit test, reset et set sur les registres ou la mémoire [HL].
//
// Référence : Zilog Z80 CPU User's Manual — Section "CB Prefix Instructions"
// ============================================================================

// ----------------------------------------------------------------------------
// z80_exec_cb
// ----------------------------------------------------------------------------
// Dispatcher du préfixe CB.
// Lit l'opcode suivant le préfixe CB, décode les champs x (2 bits), y (3 bits),
// z (3 bits) et exécute l'opération correspondante :
//   - x=0 : Rotations (RLC, RRC, RL, RR, SLA, SRA, SLL, SRL)
//   - x=1 : BIT b,r — Test de bit sans modifier le registre
//   - x=2 : RES b,r — Reset d'un bit (bit mis à 0)
//   - x=3 : SET b,r — Set d'un bit (bit mis à 1)
//
// @param cpu  Pointeur vers la structure Z80
// @return     Nombre de T-states exécutés (8 pour r≠(HL), 12/15 pour r=(HL))
// ----------------------------------------------------------------------------

int z80_exec_cb(Z80* cpu);

// ----------------------------------------------------------------------------
// z80_exec_xycb — Préfixe DD/FD CB (BIT/RES/SET/rotations sur mémoire indexée)
// ----------------------------------------------------------------------------
// Traite les instructions DD CB et FD CB : accès mémoire via IX+d ou IY+d.
// Similaire à z80_exec_cb mais l'adresse est calculée depuis *ireg + displacement.
// Les flags X/Y proviennent de WZ >> 8 (comme BIT b,(IX+d) / BIT b,(IY+d)).
// @param cpu    Pointeur vers la structure Z80
// @param ireg   Pointeur vers le registre index (IX ou IY) — modifié par INC/DEC
// @return       Nombre de T-states (23 pour rotations, 20 pour BIT, 23 pour RES/SET)
// ----------------------------------------------------------------------------

int z80_exec_xycb(Z80* cpu, uint16_t* ireg);

// ----------------------------------------------------------------------------
// z80_rot_rrca / z80_rot_rlca
// ----------------------------------------------------------------------------
// Rotations de l'accumulateur sans préfixe CB.
// Ces instructions sont exécutées directement par leur opcode (0x0F et 0x07)
// dans le décodeur principal, mais les implémentations sont regroupées ici
// car elles partagent la logique avec les rotations CB.
//
// Sur le Z80 réel :
//   - RRCA (0x0F) : Rotation circulaire droite de A. C = bit 0 → A. Flags: S=0, Z=0, H=0, PV=0, X/Y copiés depuis A, C inchangé sur certains Z80.
//   - RLCA (0x07) : Rotation circulaire gauche de A. C = bit 7 → A. Flags: S=0, Z=0, H=0, PV=0, X/Y copiés depuis A.
//
// @param cpu  Pointeur vers la structure Z80
// ----------------------------------------------------------------------------

void z80_rot_rrca(Z80* cpu);   ///< RRCA — Rotate Accumulator Right (opcode 0x0F)
void z80_rot_rlca(Z80* cpu);   ///< RLCA — Rotate Accumulator Left  (opcode 0x07)

#endif // Z80_CB_H