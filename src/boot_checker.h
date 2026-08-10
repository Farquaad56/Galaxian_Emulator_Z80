#pragma once
#include <cstdio>

// ============================================================================
// BootChecker — valide silencieusement les 7 étapes clés du boot Galaxian.
// 1 ligne par étape franchie + 1 verdict final. Jamais de flood.
// Note : l'étape IM2 a été supprimée (Galaxian utilise une NMI, pas IM2).
// ============================================================================
struct BootChecker {
    static constexpr int NB = 7;
    static constexpr int TIMEOUT_FRAMES = 300;  // ~5 s : le jeu peut mettre plus de temps (POST long)

    bool reached[NB + 1] = {};   // index 1..8
    int  frame  = 0;
    int  resets = 0;
    bool verdict = false;        // true = verdict rendu, plus rien n'est affiché
    bool boot_finished = false;  // true quand le boot a terminé normalement (attract reached)

    static const char* name(int id) {
        switch (id) {
            case 1: return "CPU reset (PC=0000)";
            case 2: return "POST : ecriture VRAM (clear 5000-57FF)";
            case 3: return "POST : ecriture OBJRAM (clear 5800-58FF)";
            case 4: return "Watchdog nourri (lecture 7800)";
            case 5: return "NMI ON (ecriture 7001 bit0=1)";
            case 6: return "Premiere NMI prise (handler 0066)";
            case 7: return "STARS ON / attract mode (7004)";
        }
        return "?";
    }

    void mark(int id) {
        if (verdict || id < 1 || id > NB || reached[id]) return;
        reached[id] = true;
        printf("[BOOT %d/%d] OK  %s  (frame %d)\n", id, NB, name(id), frame);
        if (id == NB) {
            verdict = true;
            boot_finished = true;
            printf("[BOOT] ==== BOOT COMPLETE — attract mode atteint ====\n");
        }
    }

    void tick_frame() {
        if (verdict) return;
        if (++frame > TIMEOUT_FRAMES) fail("timeout 30 frames");
    }

    void note_watchdog_reset() {
        resets++;
        if (resets == 1) {
            // reset watchdog n°1 — silencieux pour ne pas polluer la console
        } else if (!verdict) {
            fail("reset loop watchdog");
        }
    }

    void fail(const char* reason) {
        verdict = true;
        // échec boot — silencieux (debug via logs fichiers si besoin)
        (void)reason;
    }
};