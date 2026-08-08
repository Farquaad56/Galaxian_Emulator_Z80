// ============================================================================
// Galaxian Audio Synthesizer — Circuit Discret Simulation
// ============================================================================
// Simulation fidèle du hardware audio Galaxian :
//   - 3 oscillateurs 555 (tones fixes)
//   - Enveloppe RC pour FIRE (laser) et HIT (explosion)
//   - Bruit blanc basé sur le LFSR 17-bit
// ============================================================================

#pragma once
#include <cstdint>
#include <cmath>

class GalaxianAudioSynth {
public:
    // Constantes du circuit analogique Galaxian
    static constexpr float SAMPLE_RATE = 44100.0f;
    static constexpr float TAU_FIRE = 0.15f;   // Décharge RC tir (~150ms)
    static constexpr float TAU_HIT  = 0.40f;   // Décharge RC explosion (~400ms)
    static constexpr float FIRE_FREQ_MAX = 1000.0f; // Fréquence max laser (Hz)
    static constexpr float FIRE_FREQ_MIN = 200.0f;  // Fréquence min laser (Hz)
    
    // Tones fixes des oscillateurs 555
    static constexpr float TONE1_FREQ = 440.0f;   // La - Timer 1
    static constexpr float TONE2_FREQ = 330.0f;   // Mi - Timer 2
    static constexpr float TONE3_FREQ = 220.0f;   // La basse - Timer 3
    
    GalaxianAudioSynth() { reset(); }
    
    void reset() {
        m_fire_envelope = 0.0f;
        m_hit_envelope = 0.0f;
        m_master_volume = 0.5f;
        m_tone1_on = false;
        m_tone2_on = false;
        m_tone3_on = false;
        m_noise_enable = true;
        
        // Phases des oscillateurs
        m_phase[0] = 0.0f;
        m_phase[1] = 0.0f;
        m_phase[2] = 0.0f;
        m_phase[3] = 0.0f;
        
        // LFSR partagé avec le circuit vidéo (même registre 74165)
        m_lfsr = 0x1FFFF;
        
        // Pré-calcul des facteurs d'atténuation RC
        float dt = 1.0f / SAMPLE_RATE;
        m_alpha_fire = std::exp(-dt / TAU_FIRE);
        m_alpha_hit = std::exp(-dt / TAU_HIT);
    }
    
    // Contrôle hardware (écritures Z80)
    void write_control(uint16_t addr, uint8_t val) {
        uint8_t masked = addr & 0x07;
        
        switch (masked) {
            case 0: m_tone1_on = (val & 0x01) != 0; break;
            case 1: m_tone2_on = (val & 0x01) != 0; break;
            case 2: m_tone3_on = (val & 0x01) != 0; break;
            case 3: m_noise_enable = (val & 0x01) != 0; break;
            case 4: trigger_fire(); break;
            case 5: 
                // Volume bits 0-3
                m_master_volume = (val & 0x0F) / 15.0f;
                if (m_master_volume < 0.01f) m_master_volume = 0.5f; // Default
                break;
            case 6: /* Vol 2 - ignoré */ break;
            case 7: /* Noise mute - ignoré */ break;
        }
    }
    
    void trigger_fire() {
        m_fire_envelope = 1.0f;
    }
    
    void trigger_hit() {
        m_hit_envelope = 1.0f;
    }
    
    // Génère un bloc d'échantillons stéréo
    // buffer: tableau de float taille num_samples * 2 (interleaved L/R)
    void render_samples(float* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            float sample = process_sample();
            buffer[i * 2]     = sample; // Gauche
            buffer[i * 2 + 1] = sample; // Droite (mono dupliqué)
        }
    }
    
    // Met à jour le LFSR (à appeler après render_stars())
    void update_lfsr(uint32_t lfsr_value) {
        m_lfsr = lfsr_value;
    }
    
    // Accès au LFSR actuel pour synchronisation
    uint32_t get_lfsr() const { return m_lfsr; }

private:
    float process_sample() {
        float output = 0.0f;
        
        // 1. Oscillateurs tones (ondes carrées)
        if (m_tone1_on) output += step_oscillator(0, TONE1_FREQ);
        if (m_tone2_on) output += step_oscillator(1, TONE2_FREQ);
        if (m_tone3_on) output += step_oscillator(2, TONE3_FREQ);
        
        // 2. Tir FIRE (VCO modulé par enveloppe RC)
        if (m_fire_envelope > 0.001f) {
            m_fire_envelope *= m_alpha_fire;
            
            float freq = FIRE_FREQ_MIN + (FIRE_FREQ_MAX - FIRE_FREQ_MIN) * m_fire_envelope;
            output += step_oscillator(3, freq) * m_fire_envelope;
        }
        
        // 3. Bruit HIT/Explosion (LFSR + enveloppe RC)
        if (m_hit_envelope > 0.001f && m_noise_enable) {
            m_hit_envelope *= m_alpha_hit;
            
            // Mise à jour du LFSR Galaxian: x^17 + x^14 + 1
            uint32_t bit = ((m_lfsr >> 16) ^ (m_lfsr >> 13)) & 1;
            m_lfsr = (m_lfsr >> 1) | (bit << 16);
            
            float noise = (m_lfsr & 1) ? 1.0f : -1.0f;
            output += noise * m_hit_envelope * 0.5f;
        }
        
        // Mixage final avec volume master
        return output * m_master_volume * 0.2f; // Atténuation globale pour éviter le clipping
    }
    
    float step_oscillator(int idx, float freq) {
        // Avancer la phase
        m_phase[idx] += freq / SAMPLE_RATE;
        if (m_phase[idx] >= 1.0f) m_phase[idx] -= 1.0f;
        
        // Onde carrée: +1 pendant première moitié, -1 pendant seconde
        return (m_phase[idx] < 0.5f) ? 1.0f : -1.0f;
    }
    
    // États des oscillateurs
    bool m_tone1_on = false;
    bool m_tone2_on = false;
    bool m_tone3_on = false;
    bool m_noise_enable = true;
    
    // Enveloppes RC (valeur 0-1, décroissance exponentielle)
    float m_fire_envelope = 0.0f;
    float m_hit_envelope = 0.0f;
    
    // Facteurs d'atténuation pré-calculés
    float m_alpha_fire = 1.0f;
    float m_alpha_hit = 1.0f;
    
    // Volume master (0.0 - 1.0)
    float m_master_volume = 0.5f;
    
    // Phases des oscillateurs (0.0 - 1.0)
    float m_phase[4] = {0};
    
    // Registre LFSR 17-bit partagé avec circuit vidéo
    uint32_t m_lfsr = 0x1FFFF;
};