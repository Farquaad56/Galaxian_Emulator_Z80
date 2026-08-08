#ifndef Z80_OPS_H
#define Z80_OPS_H

#include "z80.h"

// ============================================================================
// Décodeur principal des opcodes non-préfixés
// Retourne le nombre de T-states exécutés
// ============================================================================

int z80_exec_main(Z80* cpu, uint8_t opcode);

#endif // Z80_OPS_H