#ifndef Z80_ED_H
#define Z80_ED_H

#include "z80.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Préfixe ED Header
// ============================================================================
//
// Ce fichier définit le dispatcher du préfixe ED qui ajoute des instructions
// spécialisées au Z80 : transferts de blocs, E/S par port C, opérations 16-bit
// avancées (ADC/SBC HL), registres spéciaux (I, R), et rotations décimales.
//
// Référence : Zilog Z80 CPU User's Manual — Section "ED Prefix Instructions"
// ============================================================================

// ----------------------------------------------------------------------------
// z80_exec_ed
// ----------------------------------------------------------------------------
// Dispatcher du préfixe ED.
// Lit l'opcode suivant le préfixe ED et exécute les instructions spécialisées
// du Z80 : ADC/SBC HL, Block Transfer (LDIR/LDDR/CPIR/CPDR), Block I/O
// (INIR/OTIR/INDR/OTDR), LD I/A/R, IM modes, IN/OUT(C), RLD/RRD, NEG, etc.
//
// @param cpu  Pointeur vers la structure Z80
// @return     Nombre de T-states exécutés (varie selon l'instruction : 5-21)
// ----------------------------------------------------------------------------

int z80_exec_ed(Z80* cpu);

#endif // Z80_ED_H