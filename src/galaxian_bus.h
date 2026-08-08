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
    bool flip_screen       = false; // 0x6003 bit0
    bool star_enable       = false; // 0x7004 bit0
    uint8_t sound_ctrl     = 0;     // 0x6004/0x6005 (ports son)
    
    // Watchdog — compteur de cycles depuis le dernier reset
    // Le Z80 lit 0x7800 pour réarmer le watchdog. Si non réarmé, reset du CPU.
    uint32_t watchdog_counter = 0;
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
    uint8_t cram [0x0400] = {}; // ✅ Color RAM (1KB, 0x5400-0x57FF)
    // 0x5800-0x5FFF → miroir OBJRAM/SPRAM (256 octets)
    // OBJRAM 512 octets : 0x5800-0x583F = attributs/scroll, 0x5840+ = sprites
    uint8_t spram[0x0200] = {};

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
        if (addr < 0x5800) return cram[addr & 0x03FF];       // ✅ CRAM (1KB, 0x5400-0x57FF)
        if (addr < 0x6000) return spram[addr & 0x00FF];      // OBJRAM/SPRAM (256B, 0x5800-0x58FF)

        // Ports d'entrée mappés en mémoire — plages complètes
        if (addr < 0x6800) return build_in0();       // 0x6000-0x67FF
        if (addr < 0x7000) return build_in1();       // 0x6800-0x6FFF
        if (addr < 0x7800) return build_in2();       // 0x7000-0x77FF

        // Watchdog — lecture réarme le compteur
        // Le Z80 lit 0x7800 pour réarmer le watchdog. Retourne toujours 0xFF.
        return 0xFF;                                 // Watchdog / inconnu — bus flottant
    }

    // ------------------------------------------------------------------------
    // Écriture mémoire
    // ------------------------------------------------------------------------
    void write(uint16_t addr, uint8_t val) {
        // ROM = read-only — pas d'interception IM2
        if (addr < 0x4000) return;
        // RAM 1KB avec mirror 0x0400
        if (addr < 0x5000) {
            ram[(addr - 0x4000) & 0x03FF] = val;
            return;
        }
        if (addr < 0x5400) { vram[addr & 0x03FF] = val; return; }   // VRAM (0x5000-0x53FF)
        if (addr < 0x5800) { cram[addr & 0x03FF] = val; return; }   // ✅ CRAM (0x5400-0x57FF)
        if (addr < 0x6000) { spram[addr & 0x00FF] = val; return; }  // AttributesRAM (0x5800-0x58FF)

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
    // Bits actifs bas : 0 = pressé/actif, 1 = relâché/inactif
    // ========================================================================
    uint8_t build_in0() const {
        uint8_t v = 0xFF;
        // Bit 0 = Coin 1 (HIGH = non inséré, LOW = pièce insérée)
        if (input.coin1)   v &= ~(1 << 0); else v |=  (1 << 0);
        // Bit 1 = Coin 2 (HIGH = non inséré, LOW = pièce insérée)
        if (input.coin2)   v &= ~(1 << 1); else v |=  (1 << 1);
        // Bit 2-3 = Joystick P1 (HIGH = relâché)
        if (input.left)    v &= ~(1 << 2); else v |=  (1 << 2);
        if (input.right)   v &= ~(1 << 3); else v |=  (1 << 3);
        // Bit 4 = Bouton tir P1 (HIGH = relâché)
        if (input.fire)    v &= ~(1 << 4); else v |=  (1 << 4);
        // Bit 5 = DIP Cabinet (0 = Upright, 1 = Cocktail)
        if (input.dipsw_cabinet) v |= (1 << 5); else v &= ~(1 << 5);

        // BUG P0 #2 CORRIGÉ : TEST et SERVICE sont actifs LOW sur le hardware Galaxian.
        // test_switch=false (défaut) → bit=1 (HIGH = OFF) — pas ON comme avant.
        // service=false (défaut) → bit=1 (HIGH = relâché) — pas pressé comme avant.
        if (input.test_switch)   v &= ~(1 << 6); else v |=  (1 << 6);
        if (input.service)       v &= ~(1 << 7); else v |=  (1 << 7);
        return v;
    }

    // ------------------------------------------------------------------------
    // build_in1 — Port IN1 (0x6800) : Start P1/P2, DIP coinage
    // ========================================================================
    uint8_t build_in1() const {
        uint8_t v = 0xFF;
        // Bit 0 = Start Player 1 (HIGH = non pressé)
        if (input.start1) v &= ~0x01;
        // Bit 1 = Start Player 2 (HIGH = non pressé)
        if (input.start2) v &= ~0x02;
        // Bits 6-7 = DIP Coinage
        // 00 = 1C/1C (défaut), 01 = 2C/1C, 10 = 1C/2C, 11 = Free Play
        uint8_t coinage = input.dipsw_coinage & 0x03;
        switch (coinage) {
            case 0: break;                              // 1C/1C — tous bits HIGH
            case 1: v &= ~(1 << 6); break;              // 2C/1C — bit 6 LOW
            case 2: v &= ~(1 << 7); break;              // 1C/2C — bit 7 LOW
            case 3: v &= ~0xC0; break;                 // Free Play : bits 6-7 LOW
            default: break;
        }
        return v;
    }

    // ------------------------------------------------------------------------
    // build_in2 — Port IN2 (0x7000) : DIP bonus life, lives
    // ========================================================================
    uint8_t build_in2() const {
        uint8_t v = 0xFF;
        // Bits 0-1 = DIP Bonus Life Score
        // 00 = 7000, 01 = 10000 (défaut), 10 = 12000, 11 = 20000
        uint8_t bonus = input.dipsw_bonus & 0x03;
        if (bonus == 1) v &= ~0x01;
        else if (bonus == 2) v &= ~0x02;
        // Bit 2 = DIP Lives (défaut hardware = 3 vies → bit=1)
        if (!input.dipsw_lives) v &= ~0x04;
        else                     v |=  0x04;
        return v;
    }

    VideoCounter   video_cnt;
    GalaxianAudioSynth audio_synth;  // Synthétiseur audio discret
    
    // BUG P1 #4 CORRIGÉ : IM2VectorTable supprimée (code mort, la ROM fait office de table)

    const VideoCounter& video_counter() const { return video_cnt; }

    // ------------------------------------------------------------------------
    // write_hw_reg — Écritures mémoire-mappées ET I/O vers les registres hardware
    // =========================================================================
    void write_hw_reg(uint16_t addr, uint8_t val) {
        // Filtre par plage d'adresse (mémoire ou I/O reconstruite)
        if (addr < 0x6000 || addr >= 0x8000) return;

        bool b0 = (val & 1) != 0;
        
        // Décodage par bits bas (A0-A3) — fonctionne pour :
        // - Adresses mémoire complètes (ex: 0x7001 & 0x0F = 0x01)
        // - Ports I/O reconstruits (ex: 0x6001 & 0x0F = 0x01)
        uint8_t port_low = static_cast<uint8_t>(addr & 0x0F);

        switch (port_low) {
            case 0x01:  // 0x7001 / 0x6001 = IRQ enable
                regs.irq_enabled = b0;
                if (!b0 && cpu_ptr) {
                    cpu_ptr->INT_line = false;
                }
                break;

            case 0x04:  // 0x7004 / 0x6004 = stars enable (MAME galaxian.cpp)
                regs.star_enable = b0;
                break;

            case 0x02:  // 0x6002 / 0x7002 = coin lockout (ignoré)
                break;

            // Flip screen X — MAME galaxian.cpp : map(0x7006, 0x7006).mirror(0x07f8)
            case 0x06:  // 0x7006 / 0x6006 = flip screen
                regs.flip_screen = b0;
                break;

            case 0x03:  // 0x6003 / 0x7003 = coin counter (ignoré)
            case 0x05:  // 0x6005 = sound control
                if (addr >= 0x6800 && addr < 0x6808) {
                    audio_synth.write_control(addr, val);
                } else {
                    regs.sound_ctrl = val;
                }
                break;

            case 0x07:  // 0x6007 / 0x7007 = start lamps (ignoré)
                break;

            default:
                // Ports sonores (0x6800-0x6807)
                if (addr >= 0x6800 && addr < 0x6808) {
                    audio_synth.write_control(addr, val);
                    uint8_t reg = addr & 0x07;
                    if (reg == 3) {
                        audio_synth.trigger_hit();
                    } else if (reg == 4) {
                        audio_synth.trigger_fire();
                    }
                }
                break;
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
    // Watchdog — réarme le compteur quand le Z80 lit 0x7800
    // ------------------------------------------------------------------------
    void reset_watchdog() {
        regs.watchdog_counter = 0;
    }

    // ------------------------------------------------------------------------
    // Incrémente le watchdog et retourne true si timeout (reset nécessaire)
    // Timeout typique : ~16ms = ~50000 cycles à 3MHz
    // Timeout diagnostic augmenté à 500000 (~160ms, 10 frames) pour permettre au boot de progresser
    // ------------------------------------------------------------------------
    bool check_watchdog(int cycles) {
        regs.watchdog_counter += cycles;
        // Timeout augmenté : ~500000 cycles ≈ 160ms (10 frames à 60Hz)
        if (regs.watchdog_counter > 500000) {
            printf("[WATCHDOG] Timeout — reset CPU (cycles=%u)\n", regs.watchdog_counter);
            return true;
        }
        return false;
    }
};
