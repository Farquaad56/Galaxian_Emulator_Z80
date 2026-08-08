#pragma once
#include "imgui.h"
#include "imgui_memory_editor.h"
#include "galaxian_emulator.h"

// ============================================================================
// DebugUI — panels ImGui de débogage pour l'émulateur Galaxian
// ============================================================================
class DebugUI {
public:
    void init(GalaxianEmulator* emu);
    void draw();  // à appeler chaque frame entre rlImGuiBegin() et rlImGuiEnd()

private:
    GalaxianEmulator* emu = nullptr;

    // Panels CPU
    void draw_cpu_panel(const Z80& cpu, const GalaxianBus& bus);

    // Panel Debug (trace, watchdog)
    void draw_debug_panel(GalaxianEmulator& emu);

    // Panel DIP Switches
    void draw_dipsw_panel(GalaxianBus& bus);

    // Panel Entrées joueur
    void draw_inputs_panel(GalaxianBus& bus);

    // Éditeur mémoire Galaxian — navigation + onglets + vue décodée
    void draw_memory_panel(GalaxianEmulator& emu, GalaxianBus& bus);

    // Vue décodée de l'OBJRAM (colonnes + sprites)
    void draw_decoded_objram(const GalaxianBus& bus);

    // Palette de couleurs (6l.bpr)
    void draw_palette_panel(const uint32_t* palette);

    // Helper : mapper une adresse Galaxian vers un onglet mémoire
    void request_memory_goto(uint16_t addr);

private:
    enum GalaxianMemTab {
        MEM_TAB_ROM = 0,
        MEM_TAB_RAM = 1,
        MEM_TAB_VRAM = 2,
        MEM_TAB_OBJ = 3,
        MEM_TAB_NONE = -1
    };

    // Instances d'imgui_memory_editor (stockées dans le header pour persistance)
    MemoryEditor mem_ed_rom;
    MemoryEditor mem_ed_ram;
    MemoryEditor mem_ed_vram;
    MemoryEditor mem_ed_sp;

    // État navigation mémoire
    char     mem_addr_input[16] = {};
    int      mem_tab_to_force = -1;
    bool     mem_goto_pending = false;
    uint16_t mem_goto_addr = 0;
    bool     mem_allow_edit = false;
    bool     mem_follow_pc = false;
};