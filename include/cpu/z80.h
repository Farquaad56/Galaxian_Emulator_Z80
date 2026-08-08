#ifndef Z80_H
#define Z80_H

#include <cstdint>

// ============================================================================
// Zilog Z80 CPU Emulator — Public Header
// ============================================================================
//
// Ce fichier définit la structure principale du CPU Z80 et toutes les fonctions
// publiques exposées par l'émulateur. Il couvre :
//   - Les macros de flags (S, Z, H, PV, N, C, X, Y)
//   - La structure Z80 avec tous ses registres
//   - Les prototypes des fonctions d'initialisation, fetch, ALU, pile, interruptions
//
// Référence : Zilog Z80 CPU User's Manual
// ============================================================================

// ----------------------------------------------------------------------------
// Macros de flags du registre F (Flags Register) du Z80
// ----------------------------------------------------------------------------
// Le registre F contient 8 flags au format bit. Les bits 3 et 5 (X, Y) sont
// des flags "undocumented" présents sur le silicium réel mais jamais documentés
// par Zilog. Ils servent notamment pour les instructions LDI/CPD et BIT.
// ----------------------------------------------------------------------------

#define FLAG_C  0x01   ///< Carry — Retenue/carry sortant du bit 7
#define FLAG_N  0x02   ///< Add/Subtract — Indique une opération SUB (posé par SUB/SBC/NEG)
#define FLAG_PV 0x04   ///< Parity/Overflow — Parité paire ou débordement arithmétique
#define FLAG_X  0x08   ///< Flag undocumented bit 3 (utilisé par LDI/CPD/BIT)
#define FLAG_H  0x10   ///< Half Carry — Retenue semi-carry entre les bits 3 et 4
#define FLAG_Y  0x20   ///< Flag undocumented bit 5 (utilisé par LDI/CPD/BIT)
#define FLAG_Z  0x40   ///< Zero — Le résultat de l'opération est nul
#define FLAG_S  0x80   ///< Sign — Bit 7 du résultat (négatif en complément à deux)

// ----------------------------------------------------------------------------
// Macro TEST_FLAG
// ----------------------------------------------------------------------------
// Teste si un flag spécifique est positionné dans le registre F.
//
// @param cpu Pointeur vers la structure Z80
// @param f   Flag à tester (FLAG_S, FLAG_Z, FLAG_H, FLAG_PV, FLAG_N, FLAG_C)
// @return Non nul si le flag est positionné, 0 sinon
// ----------------------------------------------------------------------------

#define TEST_FLAG(cpu, f)  ((cpu)->F & (f))

// ============================================================================
// Structure Z80 — Architecture matérielle complète du processeur Zilog Z80
// ============================================================================
//
// Cette structure représente l'état complet d'un CPU Z80 en mémoire. Chaque
// champ correspond à un registre ou un état interne du silicium réel.
//
// Référence : "Zilog Z80 CPU Complete Manual" (1980)
// ============================================================================

typedef struct Z80 {

    // ------------------------------------------------------------------------
    // Registres principaux (Accumulateur, Paires de registres 16-bit)
    // ------------------------------------------------------------------------
    // Le Z80 utilise des unions pour permettre l'accès aux registres en 8-bit
    // ou 16-bit. L'endianness est little-endian : le byte bas est en premier.
    // ------------------------------------------------------------------------

    union { struct { uint8_t F, A; }; uint16_t AF; };   ///< Accumulateur A + Flags F (ou AF complet)
    union { struct { uint8_t C, B; }; uint16_t BC; };   ///< Registre pair B-C (BC complet ou B,C séparés)
    union { struct { uint8_t E, D; }; uint16_t DE; };   ///< Registre pair D-E (DE complet ou D,E séparés)
    union { struct { uint8_t L, H; }; uint16_t HL; };   ///< Pointeur mémoire HL (HL complet ou H,L séparés)

    // ------------------------------------------------------------------------
    // Registres alternatifs (Shadow Registers / Alternate Set)
    // ------------------------------------------------------------------------
    // Le Z80 possède un second jeu de registres accessibles via l'instruction
    // EXX (BC', DE', HL') et EX AF,AF'. Ces registres sont utilisés par les
    // systèmes d'exploitation pour basculer rapidement de contexte.
    // ------------------------------------------------------------------------

    uint16_t AF_;   ///< Accumulateur alternatif A' + Flags F'
    uint16_t BC_;   ///< Registre pair B-C alternatif
    uint16_t DE_;   ///< Registre pair D-E alternatif
    uint16_t HL_;   ///< Pointeur mémoire alternatif

    // ------------------------------------------------------------------------
    // Registres d'index (Index Registers)
    // ------------------------------------------------------------------------
    // IX et IY sont des registres 16-bit supplémentaires introduits par le
    // Z80B. Ils remplacent HL respectivement avec les préfixes DD et FD.
    // Ils supportent l'accès indirect avec déplacement signé 8-bit : (IX+d), (IY+d).
    // ------------------------------------------------------------------------

    union { struct { uint8_t IXL, IXH; }; uint16_t IX; };   ///< Registre d'index IX (substitue HL avec préfixe DD)
    union { struct { uint8_t IYL, IYH; }; uint16_t IY; };   ///< Registre d'index IY (substitue HL avec préfixe FD)

    // ------------------------------------------------------------------------
    // Compteur programme et Stack Pointer
    // ------------------------------------------------------------------------

    uint16_t PC;              ///< Program Counter — Adresse de l'instruction suivante
    uint16_t SP;              ///< Stack Pointer — Adresse du sommet de la pile logique

    // ------------------------------------------------------------------------
    // Registres spéciaux (Special Purpose Registers)
    // ------------------------------------------------------------------------

    uint8_t  I;               ///< Interrupt Vector Register — Vecteur d'interruption IM2 (adresse base = I × 256)
    uint8_t  R;               ///< Memory Refresh Counter — Comptateur de rafraîchissement DRAM (7 bits inférieurs)
    uint16_t WZ;              ///< MEMPTR — Registre interne caché utilisé pour les adresses I/O et le calcul des flags X/Y

    // ------------------------------------------------------------------------
    // État du contrôleur d'interruptions
    // ------------------------------------------------------------------------

    bool IFF1;                ///< Flip-flop interruption principale — 1 = interruptions autorisées
    bool IFF2;                ///< Flip-flop sauvegardé — Copie de IFF1 après CALL/JMP (utilisé par RETI/RETN)
    int  IM;                  ///< Interrupt Mode — 0 (mode table), 1 (vecteur fixe 0x38), 2 (vecteur externe)
    bool halted;              ///< État HALT — 1 = CPU en pause (sortie par INT ou NMI)
    bool ei_delay;            ///< Délai EI — 1 = interruptions bloquées pendant l'instruction suivant EI
    bool INT_line;            ///< Ligne IRQ matérielle (conservée pour compatibilité, non utilisée par Galaxian)
    bool NMI_pending;         ///< Pulse NMI entre les instructions — edge-triggered, prioritaire sur INT, ne dépend pas de IFF1/IM
    bool NMI_in_service;      ///< CORRECTION (08/08/2026) : true pendant l'exécution du handler NMI. Empêche les re-détections et NMI imbriquées.

    // ------------------------------------------------------------------------
    // Callback de sortie NMI — appelé par RETN pour notifier l'hôte
    // ------------------------------------------------------------------------
    typedef void (*nmi_return_fn_t)();
    nmi_return_fn_t  nmi_return_fn;  ///< NULL = pas de callback. Appelé quand RETN est exécuté pendant NMI.

    // ------------------------------------------------------------------------
    // Callbacks mémoire et I/O (à définir par l'hôte/émulateur)
    // ------------------------------------------------------------------------
    // Ces pointeurs de fonction permettent à l'application hôte d'intercepter
    // tous les accès mémoire et ports I/O du CPU Z80.

    typedef uint8_t (*mem_read_fn_t)(uint16_t addr);   ///< Type : lecture octet depuis adresse mémoire 16-bit
    typedef void      (*mem_write_fn_t)(uint16_t addr, uint8_t value);  ///< Type : écriture octet dans mémoire 16-bit
    typedef uint8_t   (*io_read_fn_t)(uint16_t port);  ///< Type : lecture octet depuis port I/O 16-bit
    typedef void      (*io_write_fn_t)(uint16_t port, uint8_t value);  ///< Type : écriture octet vers port I/O 16-bit

    mem_read_fn_t    mem_read_fn;     ///< Fonction de lecture mémoire (NULL = accès non intercepté)
    mem_write_fn_t   mem_write_fn;    ///< Fonction d'écriture mémoire (NULL = accès non intercepté)
    io_read_fn_t     io_read_fn;      ///< Fonction de lecture I/O (NULL = accès non intercepté)
    io_write_fn_t    io_write_fn;     ///< Fonction d'écriture I/O (NULL = accès non intercepté)

    // ------------------------------------------------------------------------
    // Compteur de cycles cumulés
    // ------------------------------------------------------------------------

    int total_cycles;   ///< Nombre total de T-states (cycles machine) exécutés depuis le démarrage
} Z80;

// ============================================================================
// Fonctions publiques — Interface API de l'émulateur Z80
// ============================================================================

// ----------------------------------------------------------------------------
// z80_clear_ei_delay
// ----------------------------------------------------------------------------
// Réinitialise le délai EI (Enable Interrupts).
// Sur le Z80 réel, l'instruction EI autorise les interruptions seulement
// pendant l'instruction suivante. Cette fonction doit être appelée à la fin
// de l'exécution de l'instruction suivant un EI.
//
// @param cpu  Pointeur vers la structure Z80
// ----------------------------------------------------------------------------

inline void z80_clear_ei_delay(Z80* cpu) { cpu->ei_delay = false; }

// ----------------------------------------------------------------------------
// z80_init
// ----------------------------------------------------------------------------
// Initialise le CPU Z80 avec les valeurs par défaut du reset matériel.
//
// Après appel, l'état du CPU correspond à celui d'un Z80 fraîchement réinitialisé :
//   - AF=0x0000, BC=0x0000, DE=0x0000, HL=0x0000
//   - IX=0x0000, IY=0x0000
//   - PC=0x0000 (première instruction à l'adresse 0)
//   - SP=0xFFFF (pile pointe vers le haut de la mémoire 64Ko)
//   - I=0x00, R=0x00
//   - WZ=0x0000
//   - IFF1=false, IFF2=false, IM=0, halted=false, ei_delay=false
//   - Les callbacks mémoire/I/O sont mis à NULL
//   - total_cycles=0
//
// NOTE : Les registres alternatifs AF', BC', DE', HL' ne sont PAS initialisés
// par cette fonction car leur valeur dépend de l'hôte.
//
// @param cpu  Pointeur vers la structure Z80 à initialiser
// ----------------------------------------------------------------------------

void z80_init(Z80* cpu);

// ----------------------------------------------------------------------------
// z80_fetch_byte / z80_fetch_word
// ----------------------------------------------------------------------------
// Lecture d'octets/mots depuis la mémoire via le PC.
// Ces fonctions simulent les cycles de bus du Z80 réel :
//   - L'octet/mot est lu à l'adresse PC
//   - Le PC est incrémenté (de 1 pour byte, 2 pour word)
//   - Le registre R est rafraîchi (bit 0 basculé, bits 7-1 préservés)
//   - Le registre WZ est mis à jour (PC après incrémentation)
//
// @param cpu  Pointeur vers la structure Z80
// @return     Octet ou mot lu depuis la mémoire (little-endian pour word)
// ----------------------------------------------------------------------------

uint8_t z80_fetch_byte(Z80* cpu);
uint16_t z80_fetch_word(Z80* cpu);

// ----------------------------------------------------------------------------
// z80_read_r / z80_write_r
// ----------------------------------------------------------------------------
// Accès indirect aux registres 8-bit par index.
// L'index suit le format du champ z (3 bits) des instructions Z80 :
//   0=B, 1=C, 2=D, 3=E, 4=H, 5=L, 6=(HL), 7=A
//
// @param cpu      Pointeur vers la structure Z80
// @param index    Index registre (0-7)
// @param value    Valeur à écrire (pour write_r uniquement)
// ----------------------------------------------------------------------------

uint8_t z80_read_r(Z80* cpu, int index);
void    z80_write_r(Z80* cpu, int index, uint8_t value);

// ----------------------------------------------------------------------------
// Fonctions utilitaires de flags
// ----------------------------------------------------------------------------
// Ces fonctions calculent et positionnent les flags du registre F après
// des opérations ALU. Elles correspondent au comportement exact du silicium Z80.

/**
 * @brief Calcule la parité paire d'une valeur 8-bit.
 * @param val  Valeur 8-bit à vérifier
 * @return true si le nombre de bits à 1 est pair (parité paire), false sinon
 */
bool  z80_parity(uint8_t val);

/**
 * @brief Positionne les flags undocumented X et Y depuis une valeur.
 * Sur le Z80 réel, les bits 3 et 5 du registre F sont copiés depuis
 * les bits 3 et 5 du résultat de l'opération.
 * @param cpu  Pointeur vers la structure Z80
 * @param val  Valeur dont extraire les bits X (bit 3) et Y (bit 5)
 */
void  z80_set_flags_xy(Z80* cpu, uint8_t val);

/**
 * @brief Positionne tous les flags sauf Carry suite à une opération 8-bit.
 * Cette fonction est utilisée par les instructions qui ne modifient pas C :
 *   INC, DEC, LD r,r', et les opérations logiques (AND, OR, XOR).
 * @param cpu          Pointeur vers la structure Z80
 * @param result       Résultat 8-bit de l'opération
 * @param is_add_sub   true pour ADD/SUB (affecte N), false pour logique (efface N)
 * @param is_add       true pour ADD/INC (H=0), false pour SUB/DEC (H=1 si borrow)
 * @param carry_out    Retenue sortante du bit 7 (positionne C si true)
 * @param half_carry   Retenue semi-carry bits 3→4 (positionne H si true)
 */
void  z80_set_flags_8bit(Z80* cpu, uint8_t result, bool is_add_sub, bool is_add, bool carry_out, bool half_carry);

// ----------------------------------------------------------------------------
// Fonctions ALU — Unité Arithmétique et Logique (Accumulateur)
// ----------------------------------------------------------------------------
// Chaque fonction exécute une opération ALU sur l'accumulateur A avec la
// valeur donnée. Les flags S, Z, H, PV, N, C sont positionnés selon le
// comportement exact du Z80 réel.
//
// @param cpu  Pointeur vers la structure Z80
// @param val  Opérande 8-bit pour l'opération ALU
// ----------------------------------------------------------------------------

void z80_alu_add(Z80* cpu, uint8_t val);   ///< ADD A,val — Addition A + val (flags: S,Z,H,PV,C)
void z80_alu_adc(Z80* cpu, uint8_t val);   ///< ADC A,val — Addition avec carry A + val + C
void z80_alu_sub(Z80* cpu, uint8_t val);   ///< SUB val — Soustraction A - val (N=1, flags: S,Z,H,PV,C)
void z80_alu_sbc(Z80* cpu, uint8_t val);   ///< SBC A,val — Soustraction avec borrow A - val - C
void z80_alu_and(Z80* cpu, uint8_t val);   ///< AND val — ET logique A & val (H=1, N=0, C=0)
void z80_alu_or(Z80* cpu, uint8_t val);    ///< OR val — OU logique A | val (N=0, H=0, C=0)
void z80_alu_xor(Z80* cpu, uint8_t val);   ///< XOR val — OU exclusif A ^ val (N=0, H=0, C=0)
void z80_alu_cp(Z80* cpu, uint8_t val);    ///< CP val — Comparer A - val sans résultat (comme SUB)

// ----------------------------------------------------------------------------
// INC / DEC — Incrémenter/Décrémenter registre 8-bit
// ----------------------------------------------------------------------------
// Ces fonctions incrémentent ou décrémentent un registre 8-bit par index
// et positionnent les flags S, Z, H, PV, N comme le Z80 réel.
//
// Pour INC : N=0, H=(result_low_nibble == 0), PV=(val==0x7F)
// Pour DEC : N=1, H=(result_low_nibble == 0xF), PV=(val==0x80)
//
// @param cpu     Pointeur vers la structure Z80
// @param index   Index registre (0=B, 1=C, ..., 6=(HL), 7=A)
// ----------------------------------------------------------------------------

void z80_inc_r(Z80* cpu, int index);   ///< INC r — Incrémenter registre 8-bit par index
void z80_dec_r(Z80* cpu, int index);   ///< DEC r — Décrémenter registre 8-bit par index

// ----------------------------------------------------------------------------
// Fonctions de pile (Stack Operations)
// ----------------------------------------------------------------------------
// Le Z80 utilise une pile descendante (SP diminue lors du PUSH).
// Les opérations sont little-endian : byte bas d'abord, puis byte haut.
//
// @param cpu  Pointeur vers la structure Z80
// @param val  Valeur 16-bit à empiler (pour push) ou résultat de dépilement (pour pop)
// ----------------------------------------------------------------------------

void    z80_push_word(Z80* cpu, uint16_t val);   ///< PUSH val — Empiler un mot 16-bit (SP-=2, écrire bas,haut)
uint16_t z80_pop_word(Z80* cpu);                  ///< POP — Dépiler un mot 16-bit (lire bas,haut, SP+=2)

// ----------------------------------------------------------------------------
// Fonctions d'interruptions — INT et NMI
// ----------------------------------------------------------------------------
// Gestion des interruptions matérielles du Z80 :
//   - z80_interrupt() : appelée par l'hôte lorsqu'une interruption INT est reçue
//   - z80_nmi()       : appelée par l'hôte lorsqu'un NMI (Non-Maskable Interrupt) est reçu
//
// @param cpu        Pointeur vers la structure Z80
// @param data_bus   Valeur sur le bus de données (utilisée en IM 0 pour déterminer l'instruction)
// ----------------------------------------------------------------------------

void z80_interrupt(Z80* cpu, uint8_t data_bus);  ///< Traitement d'une interruption INT selon le mode IM
void z80_nmi(Z80* cpu);                          ///< Traitement d'un NMI (appel à RST 0x0066)

// ----------------------------------------------------------------------------
// Boucle principale Fetch-Decode-Execute
// ----------------------------------------------------------------------------
// Exécute une seule instruction Z80. Cette fonction est le cœur de l'émulateur :
//   1. Fetch — Lit l'opcode (et les opérandes) depuis la mémoire via PC
//   2. Decode — Décode l'opcode et détermine l'instruction
//   3. Execute — Exécute l'instruction et positionne les flags
//
// @param cpu  Pointeur vers la structure Z80
// @return     Nombre de T-states (cycles machine) consommés par l'instruction
//             Retourne 4 si HALT est actif, 0 en cas d'erreur fatale
// ----------------------------------------------------------------------------

int z80_step(Z80* cpu);

// ============================================================================
// Traceur d'opcodes — variables globales exposées pour le débogage
// (définies dans src/cpu/z80_ops.cpp)
// ============================================================================
#define MAX_OPCODE_TRACE 512
struct OpcodeTraceEntry {
    uint16_t pc_before;
    uint8_t  opcode;
    uint16_t pc_after;
    int      tstates;
};
extern bool g_opcode_trace_enabled;
extern int  g_opcode_trace_count;
extern OpcodeTraceEntry g_opcode_trace[];

#endif // Z80_H
