#include "galaxian_emulator.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ============================================================================
// Flag de debug ??? d??sactiv?? (??mulation stable, plus besoin de logs console)
// Pour r??activer : GALAXIAN_DEBUG_LEVEL = 1 (INFO) ou 2 (VERBOSE)
// ============================================================================
#define GALAXIAN_DEBUG_LEVEL 1

#if GALAXIAN_DEBUG_LEVEL >= 2
#define LOG_VERBOSE(...) printf(__VA_ARGS__)
#else
#define LOG_VERBOSE(...) ((void)0)
#endif

#if GALAXIAN_DEBUG_LEVEL >= 1
#define LOG_INFO(...) printf(__VA_ARGS__)
#else
#define LOG_INFO(...) ((void)0)
#endif

// ============================================================================
// Pointeur global ??? n??cessaire pour les raw function pointers C du Z80
// ============================================================================
GalaxianBus* GalaxianEmulator::g_bus_ptr = nullptr;
static GalaxianEmulator* g_emu_ptr = nullptr;

// ============================================================================
// Callbacks statiques conformes aux typedefs de z80.h
// ============================================================================
uint8_t  GalaxianEmulator::cb_mem_read (uint16_t addr) {
    uint8_t val = g_bus_ptr->read(addr);
    
    // Watchdog : r??armer le compteur quand on lit 0x7800
    if (addr >= 0x7800 && addr < 0x8000) {
        g_bus_ptr->reset_watchdog();
        if (g_emu_ptr) g_emu_ptr->boot_chk.mark(4);      // watchdog nourri
    }
    
    if (g_emu_ptr) g_emu_ptr->log_memory_access(addr, val, "R");
    return val;
}
void     GalaxianEmulator::cb_mem_write(uint16_t addr, uint8_t val) {
        g_bus_ptr->write(addr, val);
        if (g_emu_ptr) {
            g_emu_ptr->log_memory_access(addr, val, "W");
            g_emu_ptr->log_hw_reg_access(addr, val, "WRITE");
            BootChecker& bc = g_emu_ptr->boot_chk;
            if      (addr >= 0x5000 && addr < 0x5800) bc.mark(2);                    // VRAM
            else if (addr >= 0x5800 && addr < 0x6000) bc.mark(3);                    // OBJRAM
            if ((addr & 0x7800) == 0x7000) {
                if ((addr & 0x07FF) == 0x0001 && (val & 1)) bc.mark(6);              // NMI ON (mirror IO)
                if ((addr & 0x07) == 0x01 && (val & 1))      bc.mark(5);              // NMI ON écriture 7001 bit0=1
                if ((addr & 0x07) == 0x04 && (val & 1))      bc.mark(7);              // STARS ON (index 7, pas 8)
            }
        }
    }
uint8_t  GalaxianEmulator::cb_io_read  (uint16_t port) { return g_bus_ptr->io_read(port); }
void     GalaxianEmulator::cb_io_write (uint16_t port, uint8_t val) {
    // Log des ??critures I/O vers les ports hardware
    // Le Z80 envoie un port 8 bits ??? reconstruire l'adresse compl??te pour le log
    if (g_emu_ptr) {
        uint16_t addr = 0x6000 | (port & 0xFF);
        g_emu_ptr->log_hw_reg_access(addr, val, "IO");
        // Marquer NMI ON si ??criture sur 0x6001 bit0=1 (mirror de 0x7001)
        if ((addr & 0x07FF) == 0x0001 && (val & 1)) g_emu_ptr->boot_chk.mark(6);
    }
    g_bus_ptr->io_write(port, val);
}

// ============================================================================
// Constructeur ??? initialisation du Z80 avec callbacks branch??s + ouverture logs
// ============================================================================
GalaxianEmulator::GalaxianEmulator() {
    g_bus_ptr = &bus;
    g_emu_ptr = this;      // N??cessaire pour les callbacks de logging m??moire
    z80_init(&cpu);
    connect_callbacks();
    open_debug_logs(".");
}

// ============================================================================
// Destructeur ??? fermeture des fichiers de log
// ============================================================================
GalaxianEmulator::~GalaxianEmulator() {
    close_debug_logs();
}

// ============================================================================
// OUVERTURE DES FICHIERS DE LOG
// ============================================================================
void GalaxianEmulator::open_debug_logs(const char* dir) {
#ifdef LOG_BOOT_TRACE
    if (LOG_BOOT_TRACE) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/boot_opcode_trace.log", dir);
        fp_boot_trace = fopen(path, "w");
        if (fp_boot_trace) { setvbuf(fp_boot_trace, nullptr, _IONBF, 0); fputs("# Trace des 512 premiers opcodes Z80 au d??marrage\n", fp_boot_trace); fputs("# Format: CYC=xxx PC=xxxx OP=xx SP=xxxx AF=xxxx IM=x I=xx\n", fp_boot_trace); }
    }
#endif
#ifdef LOG_IRQ_EVENTS
    if (LOG_IRQ_EVENTS) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/irq_events.log", dir);
        fp_irq_events = fopen(path, "w");
        if (fp_irq_events) { setvbuf(fp_irq_events, nullptr, _IONBF, 0); fputs("# Events IRQ VBLANK ??? Trigger/Ack/Taken\n", fp_irq_events); fputs("# Format: [CYC xxxxxxx] EVENT description\n", fp_irq_events); }
    }
#endif
#ifdef LOG_HW_REG_ACCESS
    if (LOG_HW_REG_ACCESS) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/hw_reg_access.log", dir);
        fp_hw_reg_access = fopen(path, "w");
        if (fp_hw_reg_access) { setvbuf(fp_hw_reg_access, nullptr, _IONBF, 0); fputs("# Hardware access 0x6000-0x7FFF ??? ??critures Z80\n", fp_hw_reg_access); fputs("# Format: [CYC xxxxxxx] ADDR=xxxx VAL=%02X TYPE=[WRITE|IO] DESCRIPTION\n", fp_hw_reg_access); }
    }
#endif
#ifdef LOG_VRAM_SNAPSHOTS
    if (LOG_VRAM_SNAPSHOTS) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/vram_snapshots.log", dir);
        fp_vram_snapshots = fopen(path, "w");
        if (fp_vram_snapshots) { setvbuf(fp_vram_snapshots, nullptr, _IONBF, 0); fputs("# Captures VRAM/CRAM toutes les 60 frames\n", fp_vram_snapshots); fputs("# Format: FRAME=n CYC=xxxx VRAM[0..15] CRAM[0..15]\n", fp_vram_snapshots); }
    }
#endif
#ifdef LOG_CPU_STATE
    if (LOG_CPU_STATE) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/cpu_state_keymoments.log", dir);
        fp_cpu_state = fopen(path, "w");
        if (fp_cpu_state) { setvbuf(fp_cpu_state, nullptr, _IONBF, 0); fputs("# CPU state Z80 aux moments cl??s\n", fp_cpu_state); fputs("# Format: [CYC xxxxxxx] LABEL PC=xxxx AF=xxxx BC=xxxx DE=xxxx HL=xxxx SP=xxxx I=%02X IM=x F=S?Z?C?H?PV?\n", fp_cpu_state); }
    }
#endif
#ifdef LOG_SPRITES
    if (LOG_SPRITES) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/sprites_log.log", dir);
        fp_sprites = fopen(path, "w");
        if (fp_sprites) { setvbuf(fp_sprites, nullptr, _IONBF, 0); fputs("# Position et attributs des 8 sprites toutes les 60 frames\n", fp_sprites); fputs("# Format: FRAME=n CYC=xxxx SPRITE=i Y=%03d(%02X) CODE=%02X ATTR=%02X(X=%03Y:%c%c) X=%03d(%02X)\n", fp_sprites); }
    }
#endif
#ifdef LOG_MEMORY_ACCESS
    if (LOG_MEMORY_ACCESS) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/memory_access.log", dir);
        fp_memory_access = fopen(path, "w");
        if (fp_memory_access) { setvbuf(fp_memory_access, nullptr, _IONBF, 0); fputs("# Memory access Z80 ??? Lecture (R) et ??criture (W)\n", fp_memory_access); fputs("# Format: [CYC xxxxxxx] R/W ADDR=xxxx VAL=%02X\n", fp_memory_access); }
    }
#endif
#ifdef LOG_TILEMAP_DEBUG
    if (LOG_TILEMAP_DEBUG) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/tilemap_debug.log", dir);
        fp_tilemap_debug = fopen(path, "w");
        if (fp_tilemap_debug) { setvbuf(fp_tilemap_debug, nullptr, _IONBF, 0); fputs("# Analyse tuilemap ??? 32x28 tuiles avec couleurs CRAM\n", fp_tilemap_debug); fputs("# Format: FRAME=n CYC=xxxx TILE[x,y]=code:col  (x=col y=row)\n", fp_tilemap_debug); }
    }
#endif
#ifdef LOG_BOOT_SEQUENCE
    if (LOG_BOOT_SEQUENCE) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/boot_sequence.log", dir);
        fp_boot_sequence = fopen(path, "w");
        if (fp_boot_sequence) { setvbuf(fp_boot_sequence, nullptr, _IONBF, 0); fputs("# Detailed boot sequence ??? Events cl??s du POST\n", fp_boot_sequence); fputs("# Format: [CYC xxxxxxx] LABEL PC=xxxx AF=xxxx SP=xxxx I=%02X IM=x\n", fp_boot_sequence); }
    }
#endif
#ifdef LOG_RENDER_DEBUG
    if (LOG_RENDER_DEBUG) {
        char path[512]; snprintf(path, sizeof(path), "%s/Debug_log/render_debug.log", dir);
        fp_render_debug = fopen(path, "w");
        if (fp_render_debug) { setvbuf(fp_render_debug, nullptr, _IONBF, 0); fputs("# Stats de rendu par frame\n", fp_render_debug); fputs("# Format: FRAME=n CYC=xxxx PIXELS=N SPRITES_ACTIVE=N TILES_NON_EMPTY=N STARS=N EMPTY_FRAME=C\n", fp_render_debug); }
    }
#endif
}

void GalaxianEmulator::close_debug_logs() {
    if (fp_boot_trace)       { fclose(fp_boot_trace);      fp_boot_trace       = nullptr; }
    if (fp_irq_events)       { fclose(fp_irq_events);      fp_irq_events       = nullptr; }
    if (fp_hw_reg_access)    { fclose(fp_hw_reg_access);   fp_hw_reg_access    = nullptr; }
    if (fp_vram_snapshots)   { fclose(fp_vram_snapshots);  fp_vram_snapshots   = nullptr; }
    if (fp_cpu_state)        { fclose(fp_cpu_state);       fp_cpu_state        = nullptr; }
    if (fp_sprites)          { fclose(fp_sprites);         fp_sprites          = nullptr; }
    if (fp_memory_access)    { fclose(fp_memory_access);   fp_memory_access    = nullptr; }
    if (fp_tilemap_debug)    { fclose(fp_tilemap_debug);   fp_tilemap_debug    = nullptr; }
    if (fp_boot_sequence)    { fclose(fp_boot_sequence);   fp_boot_sequence    = nullptr; }
    if (fp_render_debug)     { fclose(fp_render_debug);    fp_render_debug     = nullptr; }
}

// ============================================================================
// HELPERS DE LOGGING
// ============================================================================
static inline const char* flag_str(uint8_t f, uint8_t bit, const char* s) {
    return (f & bit) ? s : "-";
}

void GalaxianEmulator::log_opcode_trace(uint16_t pc_before, uint8_t opcode, uint16_t pc_after, int tstates) {
#ifdef LOG_BOOT_TRACE
    if (!LOG_BOOT_TRACE || !fp_boot_trace || boot_trace_done) return;
    static int count = 0;
    fprintf(fp_boot_trace, "CYC=%d PC=%04X OP=%02X SP=%04X AF=%04X IM=%d I=%02X\n",
        tstates, pc_before, opcode, cpu.SP, cpu.AF, cpu.IM, cpu.I);
    if (++count >= 20000) { boot_trace_done = true; fclose(fp_boot_trace); fp_boot_trace = nullptr; }
#endif
}

void GalaxianEmulator::log_irq_event(const char* event, int cycles) {
#ifdef LOG_IRQ_EVENTS
    if (!LOG_IRQ_EVENTS || !fp_irq_events) return;
    fprintf(fp_irq_events, "[CYC %07d] %-12s", cycles, event);
    if (strcmp(event, "TRIGGER") == 0)
        fprintf(fp_irq_events, " VBLANK raised by hw (v_counter=%d, h_counter=%d)", bus.video_cnt.v_counter, bus.video_cnt.h_counter);
    else if (strcmp(event, "TAKEN") == 0)
        fprintf(fp_irq_events, " Z80 answered IRQ IM2 -> PC=%04X push SP=%04X", cpu.PC, cpu.SP);
    else if (strcmp(event, "ACK") == 0)
        fprintf(fp_irq_events, " Jeu acknowledgese IRQ via ??criture 0x7001");
    fprintf(fp_irq_events, "\n");
#endif
}

// NOTE: cette fonction ne doit JAMAIS modifier l'??tat ??mul??.
// irq_enabled est modifi?? uniquement dans write_hw_reg().
void GalaxianEmulator::log_hw_reg_access(uint16_t addr, uint8_t val, const char* type_str) {
#ifdef LOG_HW_REG_ACCESS
    if (!LOG_HW_REG_ACCESS || !fp_hw_reg_access) return;

    // Filtrer : ne logger que les registres hardware 0x6000-0x7FFF
    if (addr < 0x6000 || addr >= 0x8000) return;

    fprintf(fp_hw_reg_access, "[CYC %07d] ADDR=%04X VAL=%02X TYPE=%s", cpu.total_cycles, addr, val, type_str);

    // Décodage par région (pas de masque — comparer l'adresse complète)
    switch (addr & 0x7800) {  // 4 régions hardware
        case 0x6000:  // /DRIVER 6000-67FF
            switch (addr & 0x07) {
                case 0x01: fprintf(fp_hw_reg_access, " 2P_START_LAMP"); break;
                case 0x02: fprintf(fp_hw_reg_access, " COIN_LOCKOUT=%d", val & 1); break;
                case 0x03: fprintf(fp_hw_reg_access, " COIN_COUNTER"); break;
                case 0x04: fprintf(fp_hw_reg_access, " DAC_BIT0=%d", val & 1); break;
                case 0x05: fprintf(fp_hw_reg_access, " DAC_BIT1=%d", val & 1); break;
                case 0x06: fprintf(fp_hw_reg_access, " DAC_BIT2=%d", val & 1); break;
                case 0x07: fprintf(fp_hw_reg_access, " DAC_BIT3=%d", val & 1); break;
                default: fprintf(fp_hw_reg_access, " DRIVER_UNKNOWN"); break;
            }
            break;

        case 0x6800:  // /SOUND 6800-6FFF
            switch (addr & 0x07) {
                case 0x00: fprintf(fp_hw_reg_access, " FS1=%d", val & 1); break;
                case 0x01: fprintf(fp_hw_reg_access, " FS2=%d", val & 1); break;
                case 0x02: fprintf(fp_hw_reg_access, " FS3=%d", val & 1); break;
                case 0x03: fprintf(fp_hw_reg_access, " HIT=%d", val & 1); break;
                case 0x04: fprintf(fp_hw_reg_access, " NC"); break;
                case 0x05: fprintf(fp_hw_reg_access, " FIRE=%d", val & 1); break;
                case 0x06: fprintf(fp_hw_reg_access, " VOL1=%d", val & 1); break;
                case 0x07: fprintf(fp_hw_reg_access, " VOL2=%d", val & 1); break;
                default: fprintf(fp_hw_reg_access, " SOUND_UNKNOWN"); break;
            }
            break;

        case 0x7000:  // /LATCH 7000-77FF
            switch (addr & 0x07) {
                case 0x01: fprintf(fp_hw_reg_access, " %s", (val & 1) ? "IRQ_ENABLE_WRITE=1" : "IRQ_ENABLE_WRITE=0"); break;
                case 0x04: fprintf(fp_hw_reg_access, " STAR_ENABLE=%d", val & 1); break;
                case 0x06: fprintf(fp_hw_reg_access, " FLIP_SCREEN_X=%d", val & 1); break;
                case 0x07: fprintf(fp_hw_reg_access, " FLIP_SCREEN_Y=%d", val & 1); break;
                default: fprintf(fp_hw_reg_access, " LATCH_UNKNOWN"); break;
            }
            break;

        case 0x7800:  // /PITCH 7800-7FFF
            fprintf(fp_hw_reg_access, " PITCH=%02X", val);
            break;

        default:
            fprintf(fp_hw_reg_access, " UNKNOWN_REGION");
            break;
    }
    fprintf(fp_hw_reg_access, "\n");
#endif
}

void GalaxianEmulator::log_vram_snapshot(int frame) {
#ifdef LOG_VRAM_SNAPSHOTS
    if (!LOG_VRAM_SNAPSHOTS || !fp_vram_snapshots) return;
    fprintf(fp_vram_snapshots, "FRAME=%d CYC=%d VRAM=[", frame, cpu.total_cycles);
    for (int i = 0; i < 128 && i < 0x400; i++) {
        if (i > 0) fprintf(fp_vram_snapshots, " ");
        fprintf(fp_vram_snapshots, "%02X", bus.vram[i]);
    }
    fprintf(fp_vram_snapshots, "] SPRAM=[");
    for (int i = 0; i < 16 && i < 64; i++) {
        if (i > 0) fprintf(fp_vram_snapshots, " ");
        fprintf(fp_vram_snapshots, "%02X", bus.spram[i]);
    }
    fprintf(fp_vram_snapshots, "] star=%d\n", bus.regs.star_enable);
#endif
}

void GalaxianEmulator::log_cpu_state_moment(const char* label, int cycles) {
#ifdef LOG_CPU_STATE
    if (!LOG_CPU_STATE || !fp_cpu_state) return;
    fprintf(fp_cpu_state, "[CYC %07d] %-12s PC=%04X AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X I=%02X IM=%d F=%s%s%s%s%s\n",
        cycles, label, cpu.PC, cpu.AF, cpu.BC, cpu.DE, cpu.HL, cpu.SP, cpu.I, cpu.IM,
        flag_str(cpu.F, FLAG_S, "S"), flag_str(cpu.F, FLAG_Z, "Z"),
        flag_str(cpu.F, FLAG_C, "C"), flag_str(cpu.F, FLAG_H, "H"),
        flag_str(cpu.F, FLAG_PV, "PV"));
#endif
}

void GalaxianEmulator::log_sprites_snapshot(int frame) {
#ifdef LOG_SPRITES
    if (!LOG_SPRITES || !fp_sprites) return;
    // Hardware : sprites ?? 0x5840 (+0x40 par rapport au d??but de l'OBJRAM)
    for (int i = 0; i < 8; i++) {
        int y_off = 0x40 + i * 4;
        uint8_t sy   = bus.spram[y_off + 0];
        uint8_t code = bus.spram[y_off + 1];
        uint8_t attr = bus.spram[y_off + 2];
        uint8_t sx   = bus.spram[y_off + 3];
        // Correction MAME : les 3 premiers sprites (0-2) ont un décalage ±1
        // sy = 240 - (base0 - correction), correction=1 pour sprites 0,1,2
        int sy_correction = (i < 3) ? 1 : 0;
        int screen_y = 255 - sy + sy_correction; // M??me formule que render_sprites()
        int screen_x = sx + 1;   // Align?? sur le rendu render_sprites()
        bool flip_x  = (attr & 0x01) != 0;
        bool flip_y  = (attr & 0x02) != 0;
        int  color   = attr & 0x07;
        fprintf(fp_sprites, "FRAME=%d CYC=%d SPRITE=%d Y=%03d(%02X) CODE=%02X ATTR=%02X(X=%03d Y=%03d F:%c%c) X=%03d(%02X)\n",
            frame, cpu.total_cycles, i, sy, sy, code, attr, screen_x, screen_y,
            flip_x ? 'O' : '-', flip_y ? 'O' : '-', sx, sx);
    }
#endif
}

// ============================================================================
// connect_callbacks ??? attacher les callbacks au Z80
// z80_init() remet tous les callbacks ?? nullptr ??? toujours rebrancher !
// ============================================================================
void GalaxianEmulator::connect_callbacks() {
    cpu.mem_read_fn  = cb_mem_read;
    cpu.mem_write_fn = cb_mem_write;
    cpu.io_read_fn   = cb_io_read;
    cpu.io_write_fn  = cb_io_write;
}

// ============================================================================
// validate_ram ??? Test de validation RAM/VRAM (POST Galaxian)
// Le Z80 ??crit des patterns, relit, et boucle si mismatch.
// Retourne true if tous les tests passent.
// ============================================================================
bool GalaxianEmulator::validate_ram() {
    constexpr int RAM_SIZE = sizeof(bus.ram);
    constexpr int VRAM_SIZE = sizeof(bus.vram);
    constexpr int SPRAM_SIZE = sizeof(bus.spram);
    bool ok = true;

    // Test 1 : Pattern simple 0x55/0xAA
    for (int i = 0; i < RAM_SIZE && ok; i++) {
        bus.ram[i] = 0x55;
        if (bus.ram[i] != 0x55) ok = false;
    }
    for (int i = 0; i < RAM_SIZE && ok; i++) {
        bus.ram[i] = 0xAA;
        if (bus.ram[i] != 0xAA) ok = false;
    }

    // Test 2 : Pattern compteur
    for (int i = 0; i < RAM_SIZE && ok; i++) {
        bus.ram[i] = static_cast<uint8_t>(i & 0xFF);
        if (bus.ram[i] != (i & 0xFF)) ok = false;
    }

    // Test 3 : VRAM pattern
    for (int i = 0; i < VRAM_SIZE && ok; i++) {
        bus.vram[i] = static_cast<uint8_t>(i & 0xFF);
        if (bus.vram[i] != (i & 0xFF)) ok = false;
    }

    // Test 4 : Mirror VRAM ?? 0x5400-0x57FF ??? ??crire en VRAM, relire via mirror
    for (int i = 0; i < 0x400 && ok; i++) {
        bus.vram[i] = static_cast<uint8_t>(i & 0xFF); // ??criture directe VRAM
        if (bus.read(0x5400 + i) != (i & 0xFF)) ok = false; // lecture via mirror
    }

    // Test 5 : SPRAM pattern
    for (int i = 0; i < SPRAM_SIZE && ok; i++) {
        bus.spram[i] = static_cast<uint8_t>(i & 0xFF);
        if (bus.spram[i] != (i & 0xFF)) ok = false;
    }

    LOG_INFO("[RAM-TEST] %s ??? RAM=%d VRAM=%d (mirror 0x5400) SPRAM=%d OK\n",
        ok ? "PASS" : "FAIL", RAM_SIZE, VRAM_SIZE, SPRAM_SIZE);
    return ok;
}

// ============================================================================
// Reset ??? r??initialisation compl??te avec validation RAM + surveillance SP
// Le Z80 d??marre en IM=0, IFF1=false, I=0x00.
// Le boot Galaxian configure lui-m??me IM2 + I pendant le POST (PC???0x1B79).
// ============================================================================
void GalaxianEmulator::reset() {
    z80_init(&cpu);
    connect_callbacks(); // ??? INDISPENSABLE apr??s z80_init
    memset(bus.ram,   0, sizeof(bus.ram));
    memset(bus.vram,  0, sizeof(bus.vram));
    memset(bus.spram, 0, sizeof(bus.spram));

    // NE PAS toucher ?? bus.rom ici ??? la ROM reste intacte apr??s load_roms()

    bus.regs = HardwareRegs{};

    bus.regs.irq_enabled = false;  // NMI d??sactiv?? par d??faut (??4 MAME)


    // Reset configuration : NMI uniquement, pas d'INT (??4 MAME)
    cpu.IM   = 0;
    cpu.I    = 0x00;
    cpu.IFF1 = false;
    cpu.IFF2 = false;
    bus.regs.first_irq_triggered = false;

    star_lfsr = 0;   // Le feedback invers?? (bit12 XOR NOT bit0) ne se bloque pas ?? 0 (??6.3 MAME)
    trace_log.clear();
    boot_trace_done = false;
    boot_finished = false;
    boot_dump_done = false;
    memory_access_count = 0;
    g_opcode_trace_enabled = true;
    g_opcode_trace_count = 0;

    // Reset des compteurs debug membres
    dbg_last_i_seen     = 0xFF;
    dbg_last_im_seen    = -1;
    dbg_first_call      = true;
    dbg_run_last_im     = -1;
    dbg_run_last_i      = 0;
    dbg_frame_count     = 0;
    dbg_prev_vcounter   = -1;

    // Validation RAM/VRAM ??? canari pour d??tecter r??gression miroir avant boot Z80
    static bool ram_tested = false;
    if (!ram_tested) { validate_ram(); ram_tested = true; }

    // Lignes d'??tat : une seule fois (premier reset), pas ?? chaque reset loop
    if (boot_chk.resets == 0) {
        LOG_INFO("[RESET] PC=%04X SP=%04X IFF1=%d IM=%d I=%02X\n",
            cpu.PC, cpu.SP, cpu.IFF1 ? 1 : 0, cpu.IM, cpu.I);
        uint8_t in0 = bus.build_in0();
        LOG_INFO("[PORTS-IN] IN0=%02X IN1=%02X IN2=%02X | TEST=%s SERVICE=%s\n",
            in0, bus.build_in1(), bus.build_in2(),
            (in0 & 0x40) ? "ENFONCE" : "relache",
            (in0 & 0x80) ? "PRESSE"  : "relache");
    }

    bus.video_cnt.reset_frame();
    boot_chk.mark(1);  // CPU reset (PC=0000)
}

// ============================================================================
// Chargement des ROMs
// ============================================================================
struct RomDef { const char* name; uint8_t* dest; uint32_t offset; uint32_t size; };

bool GalaxianEmulator::load_roms(const char* dir) {
    RomDef defs[] = {
        // Programme Z80 ??? bus.rom[] (Namco Set 1)
        { "galmidw.u", bus.rom,    0x0000, 0x0800 },
        { "galmidw.v", bus.rom,    0x0800, 0x0800 },
        { "galmidw.w", bus.rom,    0x1000, 0x0800 },
        { "galmidw.y", bus.rom,    0x1800, 0x0800 },
        { "7l",        bus.rom,    0x2000, 0x0800 },  // emptyo/IRQ handler (NMI ?? 0x0066)
        // Graphismes (hors espace Z80) ??? gfx_rom[]
        { "1h.bin",    gfx_rom,    0x0000, 0x0800 },
        { "1k.bin",    gfx_rom,    0x0800, 0x0800 },
        // PROM couleurs 32 octets ??? color_prom[]
        { "6l.bpr",    color_prom, 0x0000, 0x0020 },
    };

    bool ok = true;
    for (auto& r : defs) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", dir, r.name);
        FILE* f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "[ROM] Manquant : %s\n", path);
            ok = false; continue;
        }
        size_t n = fread(r.dest + r.offset, 1, r.size, f);
        fclose(f);
        if (n != r.size) {
            fprintf(stderr, "[ROM] Taille incorrecte : %s\n", r.name);
            ok = false;
        } else {
            printf("[ROM] OK : %-12s  +0x%04X  (%u o)\n", r.name, r.offset, r.size);
        }
    }
    if (ok) {
        // =====================================================================
        // V??rification checksum ROM ??? le POST Galaxian calcule la somme des
        // octets 0x0000-0x27FF et compare avec 0x00 (test ?? 0x1B73-0x1B87)
        // Si la somme n'est pas 0, le boot bouclera en mode ??chec.
        // =====================================================================
        uint8_t rom_sum = 0;
        for (uint32_t a = 0; a < 0x2800; a++) {
            rom_sum += bus.rom[a];
        }
        if (rom_sum == 0) {
            printf("[ROM-CHECK] somme(0x0000-0x27FF) = 00 ??? ROM saine, le boot POST passera\n");
        } else {
            printf("[ROM-CHECK] WARNING somme(0x0000-0x27FF) = %02X ??? INVALID DUMP!\n", rom_sum);
            printf("[ROM-CHECK]   Le POST va loop in failure mode (VRAM[0x1F3]=01 ou 02)\n");
        }

        build_palette();
        // Remplir la zone ROM non utilis??e (0x2800-0x3FFF) avec 0xFF (bus flottant)
        memset(bus.rom + 0x2800, 0xFF, 0x1800);
    }

    // Initialiser le pointeur CPU dans le bus (pour ack IRQ)
    bus.cpu_ptr = &cpu;

    // Ne plus patcher l'ISR ??? laisser la ROM originale intacte
    return ok;
}

// ============================================================================
// Palette ??? PROM 6l.bpr ??? ARGB
// BUG 1 CORRECTED : Mapping Galaxian r??el (confirm?? par MAME galaxian.cpp)
//   bits 2:0 ??? ROUGE  (3 bits, r??sistances pond??r??es 1k/470??/220??)
//   bits 5:3 ??? VERT   (3 bits, m??mes r??sistances)
//   bits 7:6 ??? BLEU   (2 bits, r??sistances 470??/220??)
// ============================================================================
void GalaxianEmulator::build_palette() {
    // R??sistances pond??r??es normalis??es 0-255 pour 3 bits (8 niveaux)
    // Poids par conductances (1k/470/220), somme = 224 ??? MAME ??5.4
    static const uint8_t lut3[8] = { 0x00, 0x1D, 0x3E, 0x5B, 0x85, 0xA2, 0xC3, 0xE0 };
    // R??sistances pond??r??es pour 2 bits (470??/220??), somme = 224
    static const uint8_t lut2[4] = { 0x00, 0x48, 0x98, 0xE0 };

    for (int i = 0; i < 32; i++) {
        uint8_t p = color_prom[i];
        uint8_t r = lut3[(p >> 0) & 0x07];   // bits 2:0 ??? Rouge
        uint8_t g = lut3[(p >> 3) & 0x07];   // bits 5:3 ??? Vert
        uint8_t b = lut2[(p >> 6) & 0x03];   // bits 7:6 ??? Bleu
        // RGB_MAXIMUM = 224 (MAME ??5.4) ??? le plafond est dans l'alpha, pas dans les canaux
        palette[i] = (static_cast<uint32_t>(224u) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    // Palette ??toiles d??di??e 64 couleurs (??5.4/??5.5) ??? r??seau 150??/100??
    // Chaque entr??e utilise 2 octets de color_prom (paires successives)
    for (int i = 0; i < 64; i++) {
        uint8_t p = color_prom[i & 31];  // recycle les 32 octets PROM
        uint8_t r = lut3[(p >> 0) & 0x07];
        uint8_t g = lut3[(p >> 3) & 0x07];
        uint8_t b = lut2[(p >> 6) & 0x03];
        star_color[i] = (static_cast<uint32_t>(224u) << 24) | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
    }

    LOG_INFO("[PALETTE] MAME conforme ??? RGB 3-3-2 bits, RGB_MAXIMUM=224, star_color[64] construit\n");
}

// ============================================================================
// D??codage pixel de tuile ??? 2 plans de bits, 16 octets/tuile
// Hardware : 1h.bin = plan 0 (@0x000), 1k.bin = plan 1 (@0x800)
// 8 octets par tuile par plan (1 octet/ligne)
// ============================================================================
uint8_t GalaxianEmulator::decode_pixel(uint8_t tile_num, int px, int py) const {
    const int addr = (tile_num & 0xFF) * 8 + py;
    const uint8_t p0 = (gfx_rom[addr]         >> (7 - px)) & 1;
    const uint8_t p1 = (gfx_rom[addr + 0x800] >> (7 - px)) & 1;
    return (p1 << 1) | p0;                      // valeur 0???3
}

// ============================================================================
// trace_cpu_state ??? logger l'??tat du CPU dans trace_log
// NE LOGUE QU'UNE FOIS AU DEMARRAGE (pas par frame)
// ============================================================================
void GalaxianEmulator::trace_cpu_state() {
    // Ne loguer que les 50000 premiers cycles TOTAUX (pas par frame)
    if (cpu.total_cycles > 50000 * 4) return; // ~200000 T-states max
    if (trace_log.size() >= 50000) return;

    CycleTrace t;
    t.pc       = cpu.PC;
    t.opcode   = bus.read(cpu.PC);     // opcode en cours (prochain ?? ex??cuter)
    t.af       = cpu.AF;
    t.bc       = cpu.BC;
    t.de       = cpu.DE;
    t.hl       = cpu.HL;
    t.sp       = cpu.SP;
    t.total_cycles = cpu.total_cycles;
    trace_log.push_back(t);

    // Log console pour les 200 premiers cycles seulement (niveau verbose)
    if (trace_log.size() <= 200) {
        LOG_VERBOSE("[TRACE] #%03d PC=%04X OP=%02X AF=%04X BC=%04X DE=%04X HL=%04X SP=%04X CYC=%d\n",
            (int)trace_log.size(), t.pc, t.opcode, t.af, t.bc, t.de, t.hl, t.sp, t.total_cycles);
    }
}

// ============================================================================
// is_stuck ??? true si le CPU est bloqu?? dans une boucle
// ============================================================================
bool GalaxianEmulator::is_stuck() const {
    if (trace_log.size() < 50) return false;

    uint16_t last_pc = trace_log.back().pc;
    int same_count = 0;
    for (int i = (int)trace_log.size() - 1; i >= (int)trace_log.size() - 200 && i >= 0; i--) {
        if (trace_log[i].pc == last_pc) same_count++;
    }
    return same_count >= 150;
}

// ============================================================================
// run_frame ??? boucle temporelle Galaxian avec IRQ GAL84Bxx ONE-SHOT VBLANK
//
// CORRECTIONS (06/08/2026) :
//   - Bug #1 : ROM lin??aire 16 KB (plus de miroir)
//   - Bug #2 : pas d'interception IM2 dans le bus
//   - Bug #3 : detect_im2_config() supprim??e
//   - Bug #4 : reset() ??? ??tat Z80 conforme au power-on r??el
//   - Bug #8 : VBLANK ?? la ligne 224 (plus 240)
//   - Le jeu configure lui-m??me IM2 + I pendant le boot (PC???0x1B79).
// ============================================================================
void GalaxianEmulator::run_frame() {
    // Timing Galaxian exact :
    // CPU clock     : 3.072 MHz = 18.432 / 6
    // Pixel clock   : 6.144 MHz = 18.432 / 3 (2?? CPU clock)
    // V-total       : 264 scanlines (224 visibles + 40 blanking)
    // FPS           : ~60.6 Hz
    // Cycles/frame  : 3.072 MHz / 60.6 ??? 50685 T-states CPU
    // 264 lignes ?? 384 pixels / 2 (pixel clock = 2?? CPU clock) = 50688 cycles/frame
    constexpr int CYCLES_FRAME = 264 * 384 / 2; // 50688

    // Reset du compteur vid??o + vblank_triggered au d??but de chaque frame
    bus.video_cnt.reset_frame();

    // Les lignes d'interruption doivent ??tre basses en d??but de frame
    cpu.INT_line = false;
    cpu.NMI_pending = false;
    bool prev_irq_in_frame = bus.regs.irq_enabled;

    // D??tection du flip X pour recalculer l'origine LFSR ??toiles (??5.6 MAME)
    bool prev_flip_x = bus.regs.flip_screen_x;

    int cycles_done = 0;

    // Le jeu Galaxian configure lui-m??me I + IM2 + EI au boot (PC???0x1B79).
    // On ne force RIEN ??? on laisse le jeu faire son travail.

    // Debug : tracer les changements de IM et I (configuration IM2 par le jeu)
    int dbg_run_last_im_local = dbg_run_last_im;
    uint8_t dbg_run_last_i_local = dbg_run_last_i;
    int dbg_frame_count_local = dbg_frame_count;

    if (dbg_run_last_im_local == -1) {
        dbg_run_last_im_local = cpu.IM;
        dbg_run_last_i_local  = cpu.I;
    } else {
        if (cpu.IM != dbg_run_last_im_local) {
            LOG_INFO("[CPU] IM changed: %d -> %d at PC=%04X cycles=%d\n", dbg_run_last_im_local, cpu.IM, cpu.PC, cpu.total_cycles);
            log_cpu_state_moment("IM_CHANGE", cpu.total_cycles);
#ifdef LOG_BOOT_SEQUENCE
            if (!boot_finished) {
                log_boot_sequence_event("IM_CHANGED", cpu.total_cycles);
                boot_finished = true;
            }
#endif
            dbg_run_last_im_local = cpu.IM;
        }
        if (cpu.I != dbg_run_last_i_local) {
            LOG_INFO("[CPU] I changed: %02X -> %02X at PC=%04X\n", dbg_run_last_i_local, cpu.I, cpu.PC);
#ifdef LOG_BOOT_SEQUENCE
            if (!boot_finished) log_boot_sequence_event("I_REGISTER", cpu.total_cycles);
#endif
            dbg_run_last_i_local = cpu.I;
        }
    }

    while (cycles_done < CYCLES_FRAME) {

        // ------------------------------------------------------------------
        // ??tape 1 : Avancer le compteur vid??o pour un petit bloc (~50 cycles)
        //          On avance d'abord le vid??o, puis on d??tecte VBLANK.
        // ------------------------------------------------------------------
        int remaining = CYCLES_FRAME - cycles_done;
        int block_cycles = std::min(50, remaining);

        // Avancer le vid??o pour ce bloc : 1 cycle CPU = 2 cycles pixel
        bus.video_cnt.step(block_cycles * 2);

        // ------------------------------------------------------------------
        // ??tape 2 : D??tecter VBLANK ??? marquer que l'INT doit ??tre lev??e.
        // Le hardware Galaxian l??ve l'INT VBLANK quand v_counter atteint 224.
        // ------------------------------------------------------------------
        if (dbg_prev_vcounter >= 0 && bus.video_cnt.v_counter < dbg_prev_vcounter) {
            LOG_VERBOSE("[FRAME-RESET] Wrap detected\n");
        }

        // ------------------------------------------------------------------
        // ??tape 2 : D??tecter le front montant VBlank (une seule fois par frame).
        // Le hardware Galaxian l??ve une NMI au passage de la ligne 223 ?? 224.
        // La NMI ne d??pend PAS de IFF1 ??? elle est prise imm??diatement.
        //
        // NMI VBLANK : le hardware Galaxian a un flip-flop "NMI ON" (0x7001).
        // Le jeu doit explicitement activer irq_enabled via ??criture sur 0x7001
        // avant que la NMI ne soit autoris??e (conform??ment MAME ??8.1).
        bool vblank_edge_now = bus.video_cnt.take_vblank_edge();

        if (vblank_edge_now && bus.regs.irq_enabled) {
            cpu.INT_line = true;
            log_irq_event("TRIGGER", cpu.total_cycles);
        } else if (!vblank_edge_now && !prev_irq_in_frame && bus.regs.irq_enabled
                   && bus.video_cnt.vblank_active) {
            // irq_enabled passed false->true during VBLANK (game disabled/re-enabled IRQ mid-frame)
            cpu.INT_line = true;
            log_irq_event("TRIGGER", cpu.total_cycles);
        }

        prev_irq_in_frame = bus.regs.irq_enabled;
        dbg_prev_vcounter = bus.video_cnt.v_counter;

        // ------------------------------------------------------------------
        // ??tape 3 : Ex??cuter le Z80
        //          La NMI est g??r??e en premier dans z80_step() (prioritaire, ignore IFF1).
        //          Le handler NMI Galaxian est ?? l'adresse 0x0066.
        // ------------------------------------------------------------------
        uint32_t t = z80_step(&cpu);

        // --- BootChecker : IRQ VBLANK acceptée par le Z80 ---

        cpu.total_cycles += static_cast<int>(t);
        cycles_done     += static_cast<int>(t);

        // Ring buffer diagnostic reboot loop ??? 64 derni??res instructions
        ring_pc[ring_idx] = cpu.PC;
        ring_op[ring_idx] = bus.read(cpu.PC);
        ring_idx = (ring_idx + 1) & 63;

        if (cpu.PC >= 0x2000 && cpu.PC < 0x4000) seen_main = true;
        if (seen_main && cpu.PC < 0x0010) {
            seen_main = false;
        }

        // ------------------------------------------------------------------
        // ??tape 4 : Trace les 500 premiers cycles TOTAUX (debug)
        // ------------------------------------------------------------------
        if (cpu.total_cycles <= 50000 && g_opcode_trace_count < MAX_OPCODE_TRACE) {
            // Logger dans le fichier de log boot opcode (seul syst??me activ??)
            if (fp_boot_trace && !boot_trace_done) {
                log_opcode_trace(cpu.PC, bus.read(cpu.PC), cpu.PC, static_cast<int>(t));
            }
        }
    } // end of while loop

    // --- Watchdog : silencieux, verdict via BootChecker ---
    bus.tick_watchdog();
    if (bus.watchdog_timed_out()) {
        boot_chk.note_watchdog_reset();
        bus.regs.watchdog_vblanks = 0;   // r??arme, ??vite le double reset
        reset();
        return;
    }

    boot_chk.tick_frame();               // ??? timeout global 30 frames

    // Dump du ring buffer uniquement en cas d'???chec boot (pas quand attract tourne normalement)
    if (boot_chk.verdict && !boot_chk.boot_finished && !boot_dump_done) {
        boot_dump_done = true;
        printf("[BOOT] ETAT AU BLOCAGE : PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X I=%02X IM=%d\n",
               cpu.PC, cpu.SP, cpu.AF, cpu.BC, cpu.DE, cpu.HL, cpu.I, cpu.IM);
        printf("[BOOT] Boucle suspecte (24 last instructions) :\n");
        for (int k = 0; k < 24; k++) {
            int idx = (ring_idx - 24 + k + 64) & 63;
            printf("        PC=%04X OP=%02X\n", ring_pc[idx], ring_op[idx]);
        }
    }

    // Incr??menter le compteur de frames.
    dbg_frame_count_local++;
    dbg_frame_count = dbg_frame_count_local;
    log_frame_count = dbg_frame_count_local;

    // ------------------------------------------------------------------
    // Log p??riodique ??? RENDER toutes les 300 frames
    // ------------------------------------------------------------------
    if (dbg_frame_count_local > 0 && dbg_frame_count_local % 300 == 0) {
        bool has_pixels = false;
        for (int i = 0; i < 256 * 224 && !has_pixels; i++) {
            if (framebuffer[i] != 0) has_pixels = true;
        }
        LOG_INFO("[RENDER] Frame %d: framebuffer %s | VRAM[0]=%02X SPRAM[0]=%02X\n",
            dbg_frame_count_local, has_pixels ? "has pixels" : "empty", bus.vram[0], bus.spram[0]);
    }

    // Logger les snapshots p??riodiques (toutes les 60 frames)
    if (dbg_frame_count_local > 0 && dbg_frame_count_local % 60 == 0) {
        log_vram_snapshot(dbg_frame_count_local);
        log_sprites_snapshot(dbg_frame_count_local);
#ifdef LOG_TILEMAP_DEBUG
        log_tilemap_analysis(dbg_frame_count_local);
#endif
    }

    // ------------------------------------------------------------------
    // ??tape 6 : Mise ?? jour du LFSR audio (synchronis?? avec le vid??o)
    // ------------------------------------------------------------------
    bus.update_audio_lfsr(star_lfsr);

    // ------------------------------------------------------------------
    // D??tection flip X ??? recalculer l'origine LFSR ??toiles (??5.6 MAME)
    // Le nombre de clocks compt??s par frame diff??re selon le sens de balayage.
    // ------------------------------------------------------------------
    if (bus.regs.flip_screen_x != prev_flip_x) {
        bus.stars_update_origin(star_lfsr, bus.regs.flip_screen_x);
    }

    // ------------------------------------------------------------------
    // ??tape 7 : Rendu de la frame
    // ------------------------------------------------------------------
    dbg_run_last_im = dbg_run_last_im_local;
    dbg_run_last_i  = dbg_run_last_i_local;
    render_frame();
}

// ============================================================================
// render_frame ??? ordre des couches (back ??? front)
// ============================================================================
void GalaxianEmulator::render_frame() {
    if (framebuffer.size() != FB_W * FB_H) framebuffer.resize(FB_W * FB_H);
    std::fill(framebuffer.begin(), framebuffer.end(), 0u);
    render_stars();     // Couche la plus en arri??re
    render_tilemap();   // Fond de jeu
    render_bullets();   // Tirs (Shells + Missile) ??? par-dessus le fond
    render_sprites();   // Objets mobiles (par-dessus)

}

// ============================================================================
// render_stars — LFSR 17 bits : x^17 + x^14 + 1 (code MAME galaxian_v.cpp)
// P??riode : 2^17 - 1 = 131071 clocks par frame.
// Framebuffer paysage : FB_W=768 (×3 horizontal), FB_H=256.
// Le LFSR d??file le long de l'axe HORIZONTAL ; le ×3 reste horizontal.
// Damier (§5.5 MAME) : ??toiles affich??es quand (V1 XOR H8)==1 avec V1=bit1 de x,
//   H8=bit8 de y/3 (position verticale en pixels ?cran).
// ============================================================================
void GalaxianEmulator::render_stars() {
    if (!bus.regs.star_enable) return;

    uint32_t shiftreg = star_lfsr;

    for (int x = 0; x < FB_W; x++) {
        int v1 = (x >> 1) & 1;   // bit1 de la coordonn??e horizontale (V1)
        bool two = false;          // alternance : 1 sous-pixel puis 2, duty cycle 2/3
        int p = 0;                 // position courante sur l'axe vertical

        for (int clock = 0; clock < 512; clock++) {
            // Feedback LFSR selon MAME
            uint32_t feedback = ((shiftreg >> 12) ^ ~shiftreg) & 1;
            shiftreg = (shiftreg >> 1) | (feedback << 16);

            // Zone visible : H8=0 pour clock < 256, damier sur (V1 XOR H8)==1
            if (clock < 256 && v1 == 1) {
                int hlog = p / 3;    // position verticale en pixels ?cran
                if (((v1 ^ ((hlog >> 8) & 1)) == 1) && ((shiftreg & 0x1FE01) == 0x1FE00)) {
                    int color = (~shiftreg & 0x1F8) >> 3;
                    if (color < 64 && p + (two ? 2 : 1) <= FB_H) {
                        uint32_t c = star_color[color];
                        for (int k = 0; k < (two ? 2 : 1); k++)
                            framebuffer[p * FB_W + x] = c;   // ×1 sur Y, pas de ×3 vertical
                    }
                }
            }

            p += two ? 2 : 1;
            two = !two;
        }
    }
    star_lfsr = shiftreg;
}

// ============================================================================
// render_tilemap — VRAM organis??e en COLONNES d'abord : addr = col*32+row
// Framebuffer paysage : FB_W=768 (×3 horizontal), FB_H=256.
// Alignement sprites : sx = row*8+px (horizontal), sy = col*8+py-scroll_y (vertical).
// ×3 sur l'axe HORIZONTAL (pas vertical) pour les sous-pixels de tuile.
// ============================================================================
void GalaxianEmulator::render_tilemap() {
    bool fH = bus.regs.flip_screen_x;   // HFLIP → miroir axe brut X (rx)
    bool fV = bus.regs.flip_screen_y;   // VFLIP → miroir axe brut Y (ry)

    for (int col = 0; col < 32; col++) {
        int scroll_y = bus.spram[col * 2];
        uint8_t color = bus.spram[col * 2 + 1] & 0x07;

        for (int row = 0; row < 32; row++) {
            int vaddr = (col * 32 + row) & 0x03FF;
            uint8_t tile_num = bus.vram[vaddr];

            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    uint8_t pix = decode_pixel(tile_num, px, py);
                    if (pix == 0) continue;

                    // Transposition MAME : colonnes VRAM → axe V brut (ry), lignes+scroll → axe H brut (rx)
                    int rx = (row * 8 + py - scroll_y + 256) & 0xFF;  // axe H brut (scroll ici)
                    int ry = col * 8 + px;                           // axe V brut

                    if (fH) rx = 255 - rx;
                    if (fV) ry = FB_H - 1 - ry;

                    if (ry >= 224) continue;    // visible V = 224 (pas 256)

                    // ×3 sur l'axe H brut pour sous-pixels de tuile
                    int x3 = rx * 3;
                    framebuffer[ry * FB_W + x3] =
                    framebuffer[ry * FB_W + x3 + 1] =
                    framebuffer[ry * FB_W + x3 + 2] = palette[color * 4 + pix];
                }
            }
        }
    }
}

// ============================================================================
// render_sprites — 8 sprites 16??16 pixels, OBJRAM ?? 0x5840
// Framebuffer paysage : FB_W=768 (×3 horizontal), FB_H=256.
// ×3 sur l'axe HORIZONTAL pour les sous-pixels de tuile.
// ============================================================================
void GalaxianEmulator::render_sprites() {
    bool gflip_x = bus.regs.flip_screen_x;  // HFLIP → miroir axe V brut (fy) en portrait
    bool gflip_y = bus.regs.flip_screen_y;  // VFLIP → miroir axe H brut (fx) en portrait

    // Rendu de 7 ?? 0 (sprite 0 a la priorit?? haute)
    for (int i = 7; i >= 0; i--) {
        const uint8_t* s = &bus.spram[0x40 + i * 4];

        int sy_raw = static_cast<int>(s[0]);   // raw Y → axe H brut (vertical physique)
        int sx     = static_cast<int>(s[3]) + 1; // raw X → axe V brut (horizontal physique)

        // Correction MAME : les 3 premiers sprites (0-2) ont un décalage ±1
        // sy = 240 - (base0 - correction), correction=1 pour sprites 0,1,2
        int sy_correction = (i < 3) ? 1 : 0;

        // Tile index (6 bits, bits 5:0 du byte 1)
        uint8_t tile_idx = s[1] & 0x3F;

        // Attributs (byte 2) : couleur + flip flags individuels du sprite
        uint8_t attr     = s[2];
        int color        = attr & 0x07;          // bits 2:0
        bool sflipX      = (attr & 0x01) != 0;   // bit 0 → flip axe H brut (draw_py) en portrait
        bool sflipY      = (attr & 0x02) != 0;   // bit 1 → flip axe V brut (draw_px) en portrait

        // Sprite 16??16 = 4 tuiles de 8??8 arrang??es en 2??2
        // En portrait : axe H brut (rx, vertical physique) porte py/oy,
        //               axe V brut (ry, horizontal physique) porte px/ox.
        int base_tile = tile_idx * 4;
        static const int QOX[4] = {0, 0, 8, 8};   // offsets sur l'axe V brut (ry) — px direction
        static const int QOY[4] = {0, 8, 0, 8};   // offsets sur l'axe H brut (rx) — py direction

        for (int q = 0; q < 4; q++) {
            uint8_t tile_num = static_cast<uint8_t>(base_tile + q);
            int oy = QOY[q];   // offset sur l'axe H brut (rx)
            int ox = QOX[q];   // offset sur l'axe V brut (ry)

            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 8; px++) {
                    uint8_t pix = decode_pixel(tile_num, px, py);
                    if (pix == 0) continue;

                    // Flip individuel du sprite — axes transpos??s en portrait :
                    // draw_py (py) → axe H brut (rx), draw_px (px) → axe V brut (ry)
                    int draw_py = sflipX ? (7 - py) : py;   // flip sur axe H brut
                    int draw_px = sflipY ? (7 - px) : px;   // flip sur axe V brut

                    // Position brute : axe H brut (rx, ×3), axe V brut (ry)
                    // Formule MAME : sy = 240 - (base0 - correction), correction=1 pour sprites 0-2
                    int rx = sy_raw - sy_correction + oy + draw_py;  // axe H brut (vertical physique) avec correction
                    int ry = sx + ox + draw_px;             // axe V brut (horizontal physique)

                    if (gflip_x) rx = 255 - rx;            // HFLIP → miroir axe H brut
                    if (gflip_y) ry = FB_H - 1 - ry;       // VFLIP → miroir axe V brut

                    int fx = rx * 3;                        // axe H brut en sous-pixels
                    int fy = ry;                            // axe V brut

                    if (fx < 0 || fx >= 224 * 3) continue;  // visible H = 672
                    if (fy < 0 || fy >= 224) continue;      // visible V = 224

                    // Clipping hardware 17px sur l'axe H brut (§5.2 MAME)
                    if (!gflip_x) {
                        if (fx < 17 * 3) continue;          // clip.min_x = (16+1)*3
                    } else {
                        if (fx > (256 - 17) * 3 - 1) continue; // clip.max_x
                    }

                    framebuffer[fy * FB_W + fx] =
                    framebuffer[fy * FB_W + fx + 1] =
                    framebuffer[fy * FB_W + fx + 2] = palette[color * 4 + pix];
                }
            }
        }
    }
}

// ============================================================================
// render_bullets — Shells (spram[0x60-0x7C]) et Missile (spram[0x80])
// Framebuffer paysage : FB_W=768 (×3 horizontal), FB_H=256.
// ×3 sur l'axe HORIZONTAL pour les sous-pixels de tuile.
// ============================================================================
void GalaxianEmulator::render_bullets() {
    // Clipping hardware sur l'axe H brut ×3 (§5.2 MAME) : [17*3, (256-17)*3 - 1]
    int xmin_b = bus.regs.flip_screen_x ? ((256 - 17) * 3 - 1) : 17 * 3;
    int xmax_b = bus.regs.flip_screen_x ? (FB_W - 1 - 17 * 3) : (256 - 17) * 3 - 1;

    // Shells — OBJRAM base 0x60, entr??es 0 ?? 6
    for (int i = 0; i < 7; i++) {
        int base = 0x60 + i * 4;
        uint8_t sy_raw = bus.spram[base + 0];
        int fy = 255 - static_cast<int>(sy_raw);   // axe V brut (raw_y invers?? CRT)

        if (fy < 0 || fy >= 224) continue;      // visible V = 224

        // ??5.3 : x -= 4 pour alignement line buffer
        int fx_base = (static_cast<int>(bus.spram[base + 3]) - 4) * 3;   // axe H brut ×3
        if (fx_base < xmin_b || fx_base > xmax_b) continue;

        // Shell = ligne horizontale blanche pure (§5.4) — ×3 sur axe H brut
        uint32_t white = 0xFFFFFFFF;
        for (int dx = 0; dx < 12; dx++) {
            int fx = fx_base + dx * 3;
            if (fx < xmin_b || fx > xmax_b) continue;
            if (fx < 0 || fx >= 224 * 3) continue;  // visible H = 672
            framebuffer[fy * FB_W + fx] =
            framebuffer[fy * FB_W + fx + 1] =
            framebuffer[fy * FB_W + fx + 2] = white;
        }
    }

    // Missile — OBJRAM base 0x60, entr??e 7 (spram[0x80])
    {
        int base = 0x60 + 0x20;
        uint8_t sy_raw = bus.spram[base + 0];
        int fy = 255 - static_cast<int>(sy_raw);

        if (fy < 0 || fy >= 224) return;       // visible V = 224

        int fx_base = (static_cast<int>(bus.spram[base + 3]) - 4) * 3;   // axe H brut ×3
        if (fx_base < xmin_b || fx_base > xmax_b) return;

        // Missile = jaune pur (§5.4 MAME) — ×3 sur axe H brut
        uint32_t yellow = (0xFFu << 24) | (0xFFu << 16) | (0xFFu << 8) | 0x00u;
        for (int dx = 0; dx < 12; dx++) {
            int fx = fx_base + dx * 3;
            if (fx < xmin_b || fx > xmax_b) continue;
            if (fx < 0 || fx >= 224 * 3) continue;  // visible H = 672
            framebuffer[fy * FB_W + fx] =
            framebuffer[fy * FB_W + fx + 1] =
            framebuffer[fy * FB_W + fx + 2] = yellow;
        }
    }
}

// ============================================================================
// log_memory_access ??? Memory access Z80 (Lecture/??criture)
// Logu?? dans le callback cb_mem_read et cb_mem_write
// ============================================================================
void GalaxianEmulator::log_memory_access(uint16_t addr, uint8_t val, const char* type) {
#ifdef LOG_MEMORY_ACCESS
    if (!LOG_MEMORY_ACCESS || !fp_memory_access) return;
    if (memory_access_count >= 200000) return; // Limite pour ??viter saturation
    fprintf(fp_memory_access, "[CYC %07d] %c ADDR=%04X VAL=%02X\n", cpu.total_cycles, *type, addr, val);
    memory_access_count++;
#endif
}

// ============================================================================
// log_tilemap_analysis ??? Analyse compl??te de la tuilemap (32x28 tuiles)
// Logu??e toutes les 60 frames pour suivre l'??volution du framebuffer tilemap
// ============================================================================
void GalaxianEmulator::log_tilemap_analysis(int frame) {
#ifdef LOG_TILEMAP_DEBUG
    if (!LOG_TILEMAP_DEBUG || !fp_tilemap_debug) return;
    // Logger uniquement les tuiles non emptys (tile_num != 0xFF et != 0x00)
    // La couleur de chaque colonne vient de spram[col*2+1] & 0x07 (attribut OBJRAM)
    int tile_count = 0;
    for (int col = 0; col < 32; col++) {
        for (int row = 0; row < 32; row++) { // VRAM 1KB = 32 colonnes ?? 32 lignes
            int vaddr = col * 32 + row;
            uint8_t tile   = bus.vram[vaddr];
            uint8_t color  = bus.spram[col * 2 + 1] & 0x07; // Attribut colonne depuis OBJRAM (MAME galaxian_v.cpp)
            if (tile != 0x00 && tile != 0xFF) {
                fprintf(fp_tilemap_debug, "FRAME=%d CYC=%d TILE[%d,%d]=%02X:%01X\n",
                    frame, cpu.total_cycles, col, row, tile, color);
                tile_count++;
            }
        }
    }
    if (tile_count == 0) {
        fprintf(fp_tilemap_debug, "FRAME=%d CYC=%d EMPTY_TILEMAP\n", frame, cpu.total_cycles);
    }
#endif
}

// ============================================================================
// log_boot_sequence_event ??? ??v??nement critique de la s??quence de boot
// Logu?? pour suivre les changements d'??tat du CPU pendant le POST
// ============================================================================
void GalaxianEmulator::log_boot_sequence_event(const char* label, int cycles) {
#ifdef LOG_BOOT_SEQUENCE
    if (!LOG_BOOT_SEQUENCE || !fp_boot_sequence) return;
    fprintf(fp_boot_sequence, "[CYC %07d] %-20s PC=%04X AF=%04X SP=%04X I=%02X IM=%d\n",
        cycles, label, cpu.PC, cpu.AF, cpu.SP, cpu.I, cpu.IM);
#endif
}

// ============================================================================
// log_render_stats ??? Statistiques de rendu par frame
// Comptage des pixels actifs, sprites, tuiles non emptys, ??toiles
// ============================================================================
void GalaxianEmulator::log_render_stats(int frame) {
#ifdef LOG_RENDER_DEBUG
    if (!LOG_RENDER_DEBUG || !fp_render_debug) return;

    // Compter les pixels actifs dans le framebuffer
    int active_pixels = 0;
    for (int i = 0; i < 256 * 224; i++) {
        if (framebuffer[i] != 0) active_pixels++;
    }

    // Hardware : sprites ?? 0x5840 (+0x40 par rapport au d??but de l'OBJRAM)
    int active_sprites = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t code = bus.spram[0x40 + i * 4 + 1];
        if (code != 0xFF && code != 0x00) active_sprites++;
    }

    // Compter les tuiles non emptys dans la tilemap
    int empty_tiles = 0;
    for (int i = 0; i < 32 * 28; i++) {
        if (bus.vram[i] == 0x00 || bus.vram[i] == 0xFF) empty_tiles++;
    }
    int non_empty_tiles = (32 * 28) - empty_tiles;

    // D??tection ??toiles actives
    bool stars_active = bus.regs.star_enable && (star_lfsr & 0xFF) == 0xFF;

    // Frame empty ?
    bool is_empty = (active_pixels == 0);

    fprintf(fp_render_debug, "FRAME=%d CYC=%d PIXELS=%d SPRITES_ACTIVE=%d TILES_NON_EMPTY=%d STARS=%c EMPTY_FRAME=%c\n",
        frame, cpu.total_cycles, active_pixels, active_sprites, non_empty_tiles,
        stars_active ? 'Y' : 'N', is_empty ? 'Y' : 'N');
#endif
}

