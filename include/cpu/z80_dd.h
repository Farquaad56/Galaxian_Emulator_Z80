#ifndef Z80_DD_H
#define Z80_DD_H

#include "z80.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Préfixe DD / FD Header (Index Registers IX/IY)
// ============================================================================
//
// Ce fichier définit les dispatchers pour les préfixes DD et FD qui ajoutent
// les registres d'index IX et IY au Z80. Ces registres remplacent HL dans
// toutes les instructions compatibles et supportent l'accès indirect avec
// déplacement signé 8-bit : (IX+d) et (IY+d).
//
// Le préfixe DD utilise le registre IX, le préfixe FD utilise le registre IY.
// Les deux partagent la même structure de dispatcher via z80_exec_xycb().
//
// Référence : Zilog Z80B CPU User's Manual — Section "Index Registers"
// ============================================================================

// ----------------------------------------------------------------------------
// z80_exec_dd / z80_exec_fd
// ----------------------------------------------------------------------------
// Dispatchers pour les préfixes DD (IX) et FD (IY).
// Ces fonctions lisent l'opcode suivant le préfixe, substituent HL par IX/IY
// dans toutes les instructions compatibles, et exécutent l'instruction.
//
// Instructions supportées :
//   - LD IX/IY,nn / INC/DEC IX/IY / ADD IX/IY,rp
//   - LD r,(IX/IY+d) / LD (IX/IY+d),r / INC/DEC (IX/IY+d)
//   - LD IXH/IXL,r / LD r,IXH/IXL / LD IYH/IYL,r / LD r,IYH/IYL
//   - PUSH/POP IX/IY / EX (SP),IX/IY / LD SP,IX/IY
//   - JP (IX/IY) et toutes les instructions standard non modifiées
//   - Opcodes undocumented : ADD/ADC/SBC IX/IY,rp + AND/OR/XOR/CP IXH/IXL/IYH/IYL
//
// Double préfixe : DD DD, FD FD, DD ED, FD DD sont gérés (re-fetch opcode).
// Préfixe XY CB : DD CB / FD CB → appel à z80_exec_xycb().
//
// @param cpu  Pointeur vers la structure Z80
// @return     Nombre de T-states exécutés (4-23 selon l'instruction)
// ----------------------------------------------------------------------------

int z80_exec_dd(Z80* cpu);   ///< Dispatcher préfixe DD — Exécute instructions avec registre IX
int z80_exec_fd(Z80* cpu);   ///< Dispatcher préfixe FD — Exécute instructions avec registre IY

// ----------------------------------------------------------------------------
// z80_exec_xycb
// ----------------------------------------------------------------------------
// Dispatcher pour les instructions XY CB (DD CB / FD CB).
// Lit le déplacement signé 8-bit (d) après DD/FD CB, puis l'opcode CB.
// Applique l'opération CB sur la mémoire indexée [IX+d] ou [IY+d].
//
// Opérations supportées :
//   - Rotations RLC/RRC/RL/RR/SLA/SRA/SLL/SRL de (IX/IY+d)
//   - BIT b,(IX/IY+d) — Test de bit mémoire
//   - RES/SET b,(IX/IY+d) — Reset/Set bit mémoire
//
// @param cpu     Pointeur vers la structure Z80
// @param ireg    Pointeur vers le registre d'index (IX pour DD, IY pour FD)
// @return        Nombre de T-states exécutés (19-27 selon l'instruction)
// ----------------------------------------------------------------------------

int z80_exec_xycb(Z80* cpu, uint16_t* ireg);

#endif // Z80_DD_H