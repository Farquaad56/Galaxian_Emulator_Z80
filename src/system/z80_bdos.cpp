// ============================================================================
// Stub BDOS handler — non utilisé par Galaxian (originellement pour CP/M)
// ============================================================================
// Ce fichier est requis par z80.cpp mais n'est pas nécessaire pour l'émulation
// Galaxian. Les fonctions sont des stubs qui retournent false/void.

#include "system/z80_bdos.h"

static void (*print_char_fn)(char) = nullptr;
static const char* (*print_string_fn)(const char*) = nullptr;

void z80_bdos_init(void (*pc)(char), const char* (*ps)(const char*)) {
    print_char_fn = pc;
    print_string_fn = ps;
}

bool z80_handle_bdos(Z80* cpu) {
    (void)cpu;
    return false; // non géré — Galaxian n'utilise pas BDOS
}