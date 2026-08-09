#pragma once
#include "cpu/z80.h"        // Z80 Farquaad56
#include "galaxian_bus.h"
#include <vector>
#include <cstdint>
#include <cstdio>

// ============================================================================
// TRACE ENTRY — pour le logging des 200 premiers cycles
// ============================================================================
struct CycleTrace {
    uint16_t pc;
    uint8_t  opcode;
    uint16_t af, bc, de, hl;
    uint16_t sp;
    int      total_cycles;
};

// ============================================================================
// MACROS DE LOG — contrôle fin des fichiers de log
// Activer/désactiver chaque fichier individuellement
// ============================================================================
// NOTE: Tous les logs désactivés car le boot est stable et l'émulation fonctionne.
//       Réactiver temporairement pour debug en changeant 0 → 1.
#define LOG_BOOT_TRACE      0   // boot_opcode_trace.log    : 512 premiers opcodes au boot
#define LOG_IRQ_EVENTS      0   // irq_events.log           : trigger/ack VBLANK (INT correction)
#define LOG_HW_REG_ACCESS   0   // hw_reg_access.log        : écritures hardware 0x6000-0x7FFF
#define LOG_VRAM_SNAPSHOTS  0   // vram_snapshots.log       : VRAM/CRAM toutes les 60 frames
#define LOG_CPU_STATE       0   // cpu_state_keymoments.log : registres CPU aux moments clés
#define LOG_SPRITES         0   // sprites_log.log          : position sprites toutes les 60 frames
#define LOG_MEMORY_ACCESS   1   // memory_access.log        : accès mémoire Z80 (R/W)
#define LOG_TILEMAP_DEBUG   0   // tilemap_debug.log        : analyse tuilemap par frame
#define LOG_BOOT_SEQUENCE   0   // boot_sequence.log        : séquence de boot détaillée
#define LOG_RENDER_DEBUG    0   // render_debug.log         : stats rendu par frame

// ============================================================================
// GALAXIAN EMULATOR — orchestre toute l'émulation
// ============================================================================
class GalaxianEmulator {
public:
    GalaxianBus  bus;
    Z80          cpu;

    // ROM graphique et palette (indépendantes du bus Z80)
    uint8_t  gfx_rom[0x1000]      = {};   // 1h.bin + 1k.bin (4KB total)
    uint8_t  color_prom[0x20]     = {};   // 6l.bpr (32 octets)
    uint32_t palette[32]          = {};   // Palette précalculée ARGB (sprites/tuiles)
    uint32_t star_color[64]       = {};   // 64 couleurs étoiles dédiées (§5.4/§5.5)
    // Framebuffer interne élargi x3 en largeur pour le LFSR étoiles (768px)
    // Le LFSR change tous les 1.5 cycles CPU, donc chaque pixel écran correspond
    // à 3 positions consécutives du LFSR qu'il faut échantillonner séparément.
    static constexpr int FB_W = 256 * 3;   // 768 pixels internes
    static constexpr int FB_H = 224;       // hauteur conservée
    std::vector<uint32_t> framebuffer;     // Image finale (768x224) — heap, pas stack

    // Starfield LFSR 17 bits — initialisation à 0 (0x1FFFF bloque le LFSR)
    uint32_t star_lfsr = 0;

    // Debug / Trace
    std::vector<CycleTrace> trace_log;       // historique des traces (max 50000)

    // Boot sequence tracking
    bool     boot_finished    = false;       // true quand le boot a terminé (PC sort de zone POST)

    // Debug counters (membre pour permettre reset propre)
    uint8_t  dbg_last_i_seen     = 0xFF;
    int      dbg_last_im_seen    = -1;
    bool     dbg_first_call      = true;
    int      dbg_run_last_im     = -1;
    uint8_t  dbg_run_last_i      = 0;
    int      dbg_frame_count     = 0;
    int      dbg_prev_vcounter   = -1;  // wrap frame detection dans run_frame

    // Ring buffer diagnostic reboot loop — 64 dernières instructions
    uint16_t ring_pc[64]     = {};   // PC des 64 dernières instructions
    uint8_t  ring_op[64]     = {};   // opcode correspondant
    int      ring_idx        = 0;
    bool     seen_main       = false; // true si CPU a atteint la zone principale (0x2000+)

    GalaxianEmulator();
    ~GalaxianEmulator();

    bool load_roms(const char* rom_dir);
    void reset();
    void run_frame();

    // Debug helpers
    void trace_cpu_state();                          // logger un cycle dans trace_log
    bool is_stuck() const;                           // true si CPU bloqué

    // Accès direct pour ImGui (lectures sur &cpu, &bus)
    const uint32_t* get_framebuffer() const { return framebuffer.data(); }
    const uint32_t* get_palette()     const { return palette; }

    // Downsampling framebuffer x3 → écran 256px (nearest-neighbor selon H8)
    void downsample_to_screen(uint32_t* out, int h_phase) const;

private:
    // Pointeur global nécessaire pour les raw function pointers C du Z80
    static GalaxianBus* g_bus_ptr;

    // Callbacks statiques conformes aux typedefs de z80.h
    static uint8_t  cb_mem_read (uint16_t addr);
    static void     cb_mem_write(uint16_t addr, uint8_t val);
    static uint8_t  cb_io_read  (uint16_t port);
    static void     cb_io_write (uint16_t port, uint8_t val);

    void connect_callbacks();

    // Rendu vidéo
    void render_frame();
    void render_stars();
    void render_tilemap();
    void render_sprites();
    void render_bullets();  // ✅ Shells (0x60-0x7C) et Missile (0x80) — §5.2/§7 m_bullets_base=0x60

    // Palette
    void build_palette();

    // Décodage pixel de tuile (2 plans de bits, 16 octets/tuile)
    uint8_t decode_pixel(uint8_t tile_num, int px, int py) const;

    // Validation RAM/VRAM — test POST Galaxian (patterns 0x55/0xAA/compteur)
    bool validate_ram();

    // ========================================================================
    // LOGGING — gestion des fichiers de log
    // ========================================================================
    FILE* fp_boot_trace      = nullptr;
    FILE* fp_irq_events      = nullptr;
    FILE* fp_hw_reg_access   = nullptr;
    FILE* fp_vram_snapshots  = nullptr;
    FILE* fp_cpu_state       = nullptr;
    FILE* fp_sprites         = nullptr;
    FILE* fp_memory_access   = nullptr;  // accès mémoire Z80
    FILE* fp_tilemap_debug   = nullptr;  // analyse tuilemap
    FILE* fp_boot_sequence   = nullptr;  // séquence boot détaillée
    FILE* fp_render_debug    = nullptr;  // stats rendu par frame

    int  log_frame_count     = 0;       // compteur de frames pour snapshots périodiques
    bool boot_trace_done     = false;   // true après écriture du trace boot
    int  memory_access_count = 0;       // compteur d'accès mémoire (max 10000)

    void open_debug_logs(const char* dir);
    void close_debug_logs();
    void log_opcode_trace(uint16_t pc_before, uint8_t opcode, uint16_t pc_after, int tstates);
    void log_irq_event(const char* event, int cycles);
    void log_hw_reg_access(uint16_t addr, uint8_t val, const char* type_str);
    void log_vram_snapshot(int frame);
    void log_cpu_state_moment(const char* label, int cycles);
    void log_sprites_snapshot(int frame);

    // Nouveaux logs de débogage
    void log_memory_access(uint16_t addr, uint8_t val, const char* type);  // R/W accès mémoire
    void log_tilemap_analysis(int frame);                                     // analyse tuilemap par frame
    void log_boot_sequence_event(const char* label, int cycles);              // événement boot
    void log_render_stats(int frame);                                         // stats rendu par frame
};