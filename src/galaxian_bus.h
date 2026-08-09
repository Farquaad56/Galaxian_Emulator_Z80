#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include "system/galaxian_audio.h"

// ============================================================================
// Compteur vidéo hardware — H-counter + V-counter (Namco NMOS GAL84Bxx)
// ============================================================================
struct VideoCounter {
    int h_counter = 0;
    int v_counter = 0;
    bool vblank_active = false;
    bool vblank_edge = false; // true au front montant VBlank (une fois par frame)

    VideoCounter() {}

    void tick(int cycles) {
        h_counter += cycles;
        while (h_counter >= 384) {
            h_counter -= 384;
            int old_v = v_counter;
            v_counter++;
            if (v_counter >= 264) v_counter = 0;
            vblank_active = (v_counter >= 224);
            // Déclencher le front montant une seule fois par frame
            if (old_v < 224 && v_counter >= 224) {
                vblank_edge = true;
            }
        }
    }

    void step(int cycles) { tick(cycles); }

    bool take_vblank_edge() {
        bool edge = vblank_edge;
        vblank_edge = false;
        return edge;
    }

    void reset_frame() {
        h_counter = 0;
        v_counter = 0;
        vblank_active = false;
        vblank_edge = false;
    }

    bool in_irq_window() const { return (v_counter == 224 && h_counter < 10); }
};

// ============================================================================
// Registres hardware (écrits par le Z80 via 0x6000–0x7007)
// ============================================================================
struct HardwareRegs {
    bool irq_enabled       = false; // bit 0 de 0x7001 — enable IRQ VBLANK maskable
    bool first_irq_triggered = false; // true après la première IRQ (boot terminé)
    bool coin_lock         = false; // 0x6002 bit0
    bool flip_screen_x     = false; // 0x7006 — Flip screen X (mirror 0x07f8)
    bool flip_screen_y     = false; // 0x7007 — Flip screen Y (mirror 0x07f8)
    bool star_enable       = false; // 0x7004 bit0
    uint8_t sound_ctrl     = 0;     // 0x6004/0x6005 (ports son)
    
    // Watchdog — MAME : set_vblank_count("screen", 8) → reset si pas de lecture 0x7800 pendant 8 VBLANK (~132ms)
    int     watchdog_vblanks   = 0; // compteur de VBLANK depuis dernier réarmement
    static constexpr int WATCHDOG_MAX_VBLANKS = 8;
};

// ============================================================================
// Entrées joueur + DIP switches + TEST/SERVICE
// ============================================================================
struct InputState {
    bool left    = false, right  = false, fire   = false;
    bool start1  = false, start2 = false;
    bool coin1   = false, service= false;
    bool left2   = false, right2 = false, fire2  = false, coin2 = false;
    uint8_t dipsw_coinage  = 0x00;
    uint8_t dipsw_bonus    = 0x01;
    uint8_t dipsw_lives    = 0x01;   // ✅ 3 vies par défaut (bit 2=1)
    uint8_t dipsw_cabinet  = 0x00;   // ✅ Upright par défaut (bit 5=0 → IN0 bit5=0)
    bool test_switch = false;
};

// Forward declaration
struct Z80;

// ============================================================================
// Bus principal Galaxian — mémoire + I/O mappée en mémoire
// ============================================================================
class GalaxianBus {
public:
    Z80* cpu_ptr = nullptr;

    uint8_t rom  [0x4000] = {};
    // RAM 1KB avec mirror 0x0400 (MAME galaxian.cpp)
    uint8_t ram  [0x0400] = {};
    uint8_t vram [0x0400] = {};
    // La couleur des tuiles de fond vient de spram[col*2+1] & 0x07 (attribut colonne OBJRAM)
    // OBJRAM mirror(0x0700) : 256 octets décodés (MAME galaxian.cpp map(0x5800,0x58ff).mirror(0x0700))
    uint8_t spram[0x0100] = {};

    InputState   input;
    HardwareRegs regs;

    // ------------------------------------------------------------------------
    // Lecture mémoire
    // ------------------------------------------------------------------------
    uint8_t read(uint16_t addr) const {
        // ROM linéaire 16 KB — pas de miroir, pas d'interception IM2
        if (addr < 0x4000) return rom[addr];
        // RAM 1KB avec mirror 0x0400
        if (addr < 0x5000) return ram[(addr - 0x4000) & 0x03FF];
        if (addr < 0x5400) return vram[addr & 0x03FF];       // VRAM (1KB, 0x5000-0x53FF)
        // 0x5400-0x57FF : non connecté — retourne 0xFF (§3.1 MAME)
        if (addr < 0x5800) return 0xFF;
        // OBJRAM — Zone lisible et writable (0x5800-0x5FFF, mirror 0x0700 → 256 octets décodés)
        // Le CPU lit cette zone pour les collisions, le test RAM et la logique des sprites.
        if (addr < 0x6000) return spram[addr & 0x00FF];
        if (addr < 0x6800) return build_in0();       // 0x6000-0x67FF
        if (addr < 0x7000) return build_in1();       // 0x6800-0x6FFF
        if (addr < 0x7800) return build_in2();       // 0x7000-0x77FF

        // Lecture 0x7800 réarme le watchdog (MAME : watchdog_timer_device::reset_r)
        return 0xFF;
    }

    // ------------------------------------------------------------------------
    // Écriture mémoire
    // ------------------------------------------------------------------------
    void write(uint16_t addr, uint8_t val) {
        // ROM = read-only — pas d'interception IM2
        if (addr < 0x4000) return;
        // RAM 1KB avec mirror 0x0400
        if (addr < 0x4800) {
            ram[(addr - 0x4000) & 0x03FF] = val;
            return;
        }
        // 0x4800-0x4FFF : non connecté (§3.1 MAME) — ignorer l'écriture
        if (addr < 0x5000) return;
        if (addr < 0x5400) { vram[addr & 0x03FF] = val; return; }   // VRAM (0x5000-0x53FF)
        // 0x5400-0x57FF : non connecté — ignorer l'écriture (§3.1 MAME)
        if (addr < 0x5800) return;
        // OBJRAM — Écriture mémoire-synchro (0x5800-0x5FFF, mirror 0x0700 → 256 octets)
        if (addr < 0x6000) { spram[addr & 0x00FF] = val; return; }   // OBJRAM/SPRAM writable

        write_hw_reg(addr, val);
    }

    // ------------------------------------------------------------------------
    // I/O — instructions IN / OUT
    // ------------------------------------------------------------------------
    uint8_t io_read(uint16_t port) const {
        // Le Z80 envoie un port 8 bits (n) dans IN A,(n) / OUT (n),A
        // Reconstruire l'adresse mémoire complète : base 0x6000 + port bas
        uint16_t addr = 0x6000 | (port & 0xFF);
        
        if (addr < 0x6800) return build_in0();       // 0x6000-0x67FF
        if (addr < 0x7000) return build_in1();       // 0x6800-0x6FFF
        if (addr < 0x7800) return build_in2();       // 0x7000-0x77FF
        return 0xFF;                                 // Watchdog / inconnu — bus flottant
    }

    void io_write(uint16_t port, uint8_t val) {
        // Le Z80 envoie un port 8 bits (n) dans OUT (n),A
        // Reconstruire l'adresse mémoire complète : base 0x6000 + port bas
        uint16_t addr = 0x6000 | (port & 0xFF);
        write_hw_reg(addr, val);
    }

public:
    // ------------------------------------------------------------------------
    // build_in0 — Port IN0 (0x6000) : Coin1, Coin2, Joystick P1, DIP cabinet, TEST, SERVICE
    // MAME galaxian.cpp : tous les bits sont IP_ACTIVE_HIGH → base 0x00, on positionne le bit quand actif.
    // ========================================================================
    uint8_t build_in0() const {
        uint8_t v = 0x00;
        // Bit 0 = Coin1 (IP_ACTIVE_HIGH)
        if (input.coin1)   v |= (1 << 0);
        // Bit 1 = Coin2 (IP_ACTIVE_HIGH)
        if (input.coin2)   v |= (1 << 1);
        // Bit 2 = Joystick Left P1 (IP_ACTIVE_HIGH)
        if (input.left)    v |= (1 << 2);
        // Bit 3 = Joystick Right P1 (IP_ACTIVE_HIGH)
        if (input.right)   v |= (1 << 3);
        // Bit 4 = Bouton tir P1 (IP_ACTIVE_HIGH)
        if (input.fire)    v |= (1 << 4);
        // Bit 5 = DIP Cabinet (0=Upright, 1=Cocktail) — valeur brute du switch
        if (input.dipsw_cabinet) v |= (1 << 5);
        // Bit 6 = TEST (IP_ACTIVE_HIGH)
        if (input.test_switch)   v |= (1 << 6);
        // Bit 7 = SERVICE (IP_ACTIVE_HIGH)
        if (input.service)       v |= (1 << 7);
        return v;
    }

    // ------------------------------------------------------------------------
    // build_in1 — Port IN1 (0x6800) : Start P1/P2, DIP coinage
    // MAME galaxian.cpp : bits Start IP_ACTIVE_HIGH, DIP coinage = valeurs brutes.
    // ========================================================================
    uint8_t build_in1() const {
        uint8_t v = 0x00;
        // Bit 0 = Start Player 1 (IP_ACTIVE_HIGH)
        if (input.start1) v |= 0x01;
        // Bit 1 = Start Player 2 (IP_ACTIVE_HIGH)
        if (input.start2) v |= 0x02;
        // Bits 6-7 = DIP Coinage — valeurs brutes MAME :
        // 00=1C/1C(défaut), 01=2C/1C, 10=1C/2C, 11=Free Play
        uint8_t coinage = input.dipsw_coinage & 0x03;
        v |= (coinage << 6);
        return v;
    }

    // ------------------------------------------------------------------------
    // build_in2 — Port IN2 (0x7000) : DIP bonus life, lives
    // MAME galaxian.cpp : bits bonus/lives = valeurs brutes.
    // ========================================================================
    uint8_t build_in2() const {
        uint8_t v = 0x00;
        // Bits 0-1 = DIP Bonus Life Score — valeurs brutes MAME :
        // 00=7000, 01=10000(défaut), 10=12000, 11=20000
        uint8_t bonus = input.dipsw_bonus & 0x03;
        v |= bonus;
        // Bit 2 = DIP Lives — défaut hardware = 3 vies (bit=1)
        if (input.dipsw_lives) v |= 0x04;
        return v;
    }

    VideoCounter   video_cnt;
    GalaxianAudioSynth audio_synth;  // Synthétiseur audio discret
    
    const VideoCounter& video_counter() const { return video_cnt; }

    // ------------------------------------------------------------------------
    // write_hw_reg — Écritures mémoire-mappées ET I/O vers les registres hardware
    // Décodage séparé /DRIVER (0x6xxx) et /LATCH (0x7xxx) pour éviter collisions.
    // MAME galaxian.cpp : deux chip-selects distincts sur le vrai hardware.
    // =========================================================================
    void write_hw_reg(uint16_t addr, uint8_t val) {
        // Filtre par plage d'adresse (mémoire ou I/O reconstruite)
        if (addr < 0x6000 || addr >= 0x8000) return;

        bool b0 = (val & 1) != 0;
        uint8_t port_low = static_cast<uint8_t>(addr & 0x0F);
        bool is_latch = (addr & 0x0800) != 0; // true pour 0x7xxx, false pour 0x6xxx

        if (is_latch) {
            // === Régions /LATCH (0x7000-0x77FF) === MAME galaxian.cpp
            switch (port_low) {
                case 0x01:  // 0x7001 = NMI ON (flip-flop D, actif HIGH) — MAME galaxian.cpp irq_enable_w
                    regs.irq_enabled = b0;
                    // Galaxian câbine la ligne VBLANK sur NMI (INPUT_LINE_NMI), pas INT.
                    // La désactivation ne touche pas au flag NMI_pending (géré par z80_step).
                    break;
                case 0x04:  // 0x7004 = Stars enable
                    regs.star_enable = b0;
                    break;
                case 0x06:  // 0x7006 = Flip screen X (mirror 0x07f8)
                    regs.flip_screen_x = b0;
                    break;
                case 0x07:  // 0x7007 = Flip screen Y (mirror 0x07f8)
                    regs.flip_screen_y = b0;
                    break;
                default:
                    break;
            }
        } else {
            // === Régions /DRIVER (0x6000-0x67FF) === MAME galaxian.cpp
            switch (port_low) {
                case 0x01:  // 0x6001 = 2P START LAMP (ignoré sur hardware)
                    break;
                case 0x02:  // 0x6002 = Coin lockout
                    regs.coin_lock = b0;
                    break;
                case 0x03:  // 0x6003 = Coin counter (ignoré)
                    break;
                case 0x04:  // 0x6004 = DAC bit 0 (1MΩ) → VCO fond sonore
                    audio_synth.write_dac(0, b0); break;
                case 0x05:  // 0x6005 = DAC bit 1 (470kΩ) → VCO fond sonore
                    audio_synth.write_dac(1, b0); break;
                case 0x06:  // 0x6006 = DAC bit 2 (220kΩ) → VCO fond sonore
                    audio_synth.write_dac(2, b0); break;
                case 0x07:  // 0x6007 = Start lamps (ignoré)
                    break;
                default:
                    break;
            }
        }

        // Ports sonores (0x6800-0x6807) — MAME galaxian.cpp §3.3
        // 6800=FS1, 6801=FS2, 6802=FS3, 6803=HIT, 6804=n/c, 6805=FIRE, 6806=VOL1, 6807=VOL2
        if (addr >= 0x6800 && addr < 0x6808) {
            audio_synth.write_control(addr, val);
            uint8_t reg = addr & 0x07;
            if (reg == 3)       audio_synth.trigger_hit();   // HIT
            else if (reg == 5)  audio_synth.trigger_fire();  // FIRE (6804=n/c ignoré)
        }

        // Pitch register (0x7800) — conservé pour compatibilité
        if ((addr & 0x07FF) == 0x7800) {
            float pitch_factor = 1.0f + (val & 0x0F) * 0.05f;
            (void)pitch_factor;
        }
    }

public:
    // Génère les échantillons audio pour cette frame
    void render_audio(float* buffer, int num_samples) {
        audio_synth.render_samples(buffer, num_samples);
    }
    
    // Met à jour le LFSR audio après le rendu vidéo
    void update_audio_lfsr(uint32_t lfsr) {
        audio_synth.update_lfsr(lfsr);
    }

    // ------------------------------------------------------------------------
    // Recalcul de l'origine du LFSR étoiles — MAME §5.6 galaxian_flip_screen_x_w
    // Indispensable avant tout flip screen : le nombre de clocks comptés par
    // frame diffère selon le sens de balayage, sans quoi les étoiles se
    // désynchronisent instantanément à l'écran (§5.5).
    // ------------------------------------------------------------------------
    void stars_update_origin(uint32_t &star_lfsr, bool flip_x) {
        // Période LFSR = 2^17 - 1 = 131071 clocks par frame (MAME §5.5)
        constexpr int STAR_RNG_PERIOD = ((1 << 17) - 1);
        uint32_t shiftreg = star_lfsr;
        // Avancer le LFSR d'un cycle supplémentaire selon le flip pour compenser
        // le décalage de 0 ou 2 clocks/frame dû aux bascules D en 6B (§5.5)
        int extra_cycles = flip_x ? 1 : 3;
        for (int c = 0; c < extra_cycles; c++) {
            uint32_t feedback = ((shiftreg >> 12) ^ ~shiftreg) & 1;
            shiftreg = (shiftreg >> 1) | (feedback << 16);
        }
        star_lfsr = shiftreg;
    }

    // ------------------------------------------------------------------------
    // Watchdog — MAME : set_vblank_count("screen", 8)
    // Le Z80 lit 0x7800 pour réarmer. Reset si 8 VBLANK sans lecture.
    // ------------------------------------------------------------------------
    void reset_watchdog() {
        regs.watchdog_vblanks = 0;
    }

    // Appelée à chaque front montant VBLANK (une fois par frame)
    void tick_watchdog() {
        regs.watchdog_vblanks++;
        if (regs.watchdog_vblanks >= HardwareRegs::WATCHDOG_MAX_VBLANKS) {
            printf("[WATCHDOG] Timeout — reset CPU (%d VBLANKs sans lecture 0x7800)\n", regs.watchdog_vblanks);
        }
    }

    bool watchdog_timed_out() const {
        return regs.watchdog_vblanks >= HardwareRegs::WATCHDOG_MAX_VBLANKS;
    }
};
