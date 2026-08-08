#ifndef Z80_FD_H
#define Z80_FD_H

#include "z80.h"

// ============================================================================
// Zilog Z80 CPU Emulator — Préfixe FD Header (IY Index Register)
// ============================================================================
//
// Ce fichier définit le dispatcher du préfixe FD qui active le registre d'index
// IY sur le Z80B. Le préfixe FD est structurellement identique au préfixe DD
// mais utilise IY au lieu de IX.
//
// Les détails complets sont documentés dans z80_dd.h (les deux dispatchers
// partagent la même architecture).
//
// Référence : Zilog Z80B CPU User's Manual — Section "Index Registers"
// ============================================================================

/**
 * @brief Dispatcher du préfixe FD — Exécute instructions avec registre IY.
 * 
 * Lit l'opcode suivant le préfixe FD, substitue HL par IY dans toutes les
 * instructions compatibles, et exécute l'instruction. Supporte également
 * les double préfixes (FD FD, FD DD, FD ED) et le sous-préfixe FD CB.
 * 
 * @param cpu  Pointeur vers la structure Z80
 * @return     Nombre de T-states exécutés (4-27 selon l'instruction)
 */
int z80_exec_fd(Z80* cpu);

#endif // Z80_FD_H