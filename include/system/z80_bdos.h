#ifndef Z80_BDOS_H
#define Z80_BDOS_H

#include "cpu/z80.h"

// ============================================================================
// Handler BDOS pour emulation CP/M — Validation ZEXALL/ZEXDOC
// ============================================================================

/**
 * Initialiser le handler BDOS avec les callbacks d'affichage.
 */
void z80_bdos_init(void (*print_char)(char), const char* (*print_string)(const char*));

/**
 * Gerer les appels BDOS (CP/M) pour ZEXALL/ZEXDOC.
 * Retourne true si l'appel a ete gere, false sinon.
 */
bool z80_handle_bdos(Z80* cpu);

#endif // Z80_BDOS_H